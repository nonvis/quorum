#pragma once

// Phase 11 — `quorum librarian curate` CLI. Drives the librarian-as-curator
// cycle defined in templates/specs/pitch-protocol.md (v0.1).
//
// The librarian runs ANALYST-CLASS (read-only). It NEVER writes files itself —
// it emits CURATION_UPDATE / DECISION_LOG_APPEND blocks and the daemon performs
// all writes through the Batch A primitives in vault/librarian_curator.h, behind
// an operator-approval diff gate. This mirrors the scribe LEARNINGS_UPDATE path.
//
// ARCHITECTURE: the testable pipeline (parse -> validate -> diff -> apply) is a
// PURE function (run_curation_pipeline) that takes the librarian's raw output as
// a string and spends NO claude tokens. The single live `claude -p` invocation
// lives ONLY in run_librarian_curate, modelled on cli/agent_create.h's
// lightweight synchronous analyst-clamped pattern. This separation lets the
// pipeline be unit-tested with canned librarian output.
//
// Header-only, matches the cli/vault_dedup.h / cli/agent_create.h convention.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "agent/output_parser.h"          // OutputParser, ParsedOutput
#include "vault/librarian_curator.h"      // ensure_curation_skeleton, apply_* primitives
#include "vault/scribe_writer.h"          // detail::read_file_text
#include "utils/subprocess.h"             // run_command
#include "utils/json.h"                   // json::extract_string

namespace sui::quorum::cli {

struct LibrarianCurateOptions {
    std::string project_path;   // resolved target project root (sibling of .quorum/)
    bool dry_run = false;       // --dry-run: preview only, no writes
    bool apply_all = false;     // --apply: write all proposals without prompting
    std::string model;          // --model <m>: claude model for the librarian
                                // invoke (empty = claude default). Curation is a
                                // light distill+route job, so sonnet is apt.
};

// How the pipeline treats each validated proposal.
//   PreviewOnly  — compute diffs, never write (the --dry-run path).
//   ApplyAll     — write every proposal (the --apply path).
//   Interactive  — compute diffs, never write; the command-entry operator gate
//                  decides per-proposal and re-runs apply for approved ones.
enum class ApplyMode { PreviewOnly, ApplyAll, Interactive };

// One proposal's outcome in a CurationPlan. `kind` distinguishes the block type.
// `target` is "<file> :: ## <section>" for curation updates, or
// "00 - Decision Log.md :: <decision-first-line>" for decision appends.
struct CurationProposal {
    enum class Kind { CurationUpdate, DecisionLogAppend };
    Kind kind;
    std::string file;       // canonical output file the block targets
    std::string target;     // human label (section heading or decision title)
    std::string diff;       // unified-ish diff of the change
    std::string status;     // "applied" | "previewed" | "skipped" | "rejected"
    std::string reason;     // diagnostic when rejected/skipped

    // Retained so an Interactive command-entry gate can apply approved
    // proposals after preview without re-parsing.
    CurationUpdate curation_update;        // valid when kind == CurationUpdate
    DecisionLogAppend decision_append;     // valid when kind == DecisionLogAppend
};

struct CurationPlan {
    std::vector<CurationProposal> proposals;

    [[nodiscard]] size_t applied_count() const {
        return std::count_if(proposals.begin(), proposals.end(),
            [](const CurationProposal& p) { return p.status == "applied"; });
    }
    [[nodiscard]] size_t rejected_count() const {
        return std::count_if(proposals.begin(), proposals.end(),
            [](const CurationProposal& p) { return p.status == "rejected"; });
    }
    [[nodiscard]] size_t skipped_count() const {
        return std::count_if(proposals.begin(), proposals.end(),
            [](const CurationProposal& p) { return p.status == "skipped"; });
    }
};

namespace detail {

// Read a file fully into a string; empty string if absent / unreadable.
[[nodiscard]] inline std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p);
    if (!in.is_open()) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Append "## <label>\n<contents-or-(absent)>\n" framing for the prompt.
inline void append_file_section(std::string& out, const std::string& label,
                                const std::filesystem::path& p) {
    out += "### " + label + " (" + p.filename().string() + ")\n\n";
    auto content = slurp(p);
    if (content.empty()) {
        out += "(file absent or empty — will be bootstrapped)\n\n";
    } else {
        out += "```\n" + content;
        if (content.back() != '\n') out += "\n";
        out += "```\n\n";
    }
}

// Build a digest of the scribe knowledge vault: filenames plus the head of the
// most-recently-modified notes. Bounded so the prompt stays compact. Avoids
// pulling in context_assembler.h (later-batch surface) — plain filesystem only.
[[nodiscard]] inline std::string scribe_vault_digest(
    const std::filesystem::path& knowledge_dir, size_t max_recent = 5,
    size_t head_chars = 800) {
    namespace fs = std::filesystem;
    std::string out;
    if (!fs::exists(knowledge_dir) || !fs::is_directory(knowledge_dir)) {
        out += "(no scribe knowledge vault found)\n";
        return out;
    }

    struct Note {
        fs::path path;
        fs::file_time_type mtime;
    };
    std::vector<Note> notes;
    for (const auto& e : fs::directory_iterator(knowledge_dir)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".md") continue;
        std::error_code ec;
        auto mt = fs::last_write_time(e.path(), ec);
        notes.push_back({e.path(), mt});
    }
    if (notes.empty()) {
        out += "(scribe knowledge vault is empty)\n";
        return out;
    }

    // Filenames first (deterministic order).
    std::sort(notes.begin(), notes.end(),
        [](const Note& a, const Note& b) {
            return a.path.filename().string() < b.path.filename().string();
        });
    out += "Filenames:\n";
    for (const auto& n : notes) {
        out += "- " + n.path.filename().string() + "\n";
    }
    out += "\n";

    // Most-recent notes (by mtime) with a bounded head excerpt.
    std::sort(notes.begin(), notes.end(),
        [](const Note& a, const Note& b) { return a.mtime > b.mtime; });
    size_t shown = std::min(max_recent, notes.size());
    out += "Recent notes (head):\n\n";
    for (size_t i = 0; i < shown; ++i) {
        auto content = slurp(notes[i].path);
        if (content.size() > head_chars) {
            content = content.substr(0, head_chars) + "\n... (truncated)";
        }
        out += "#### " + notes[i].path.filename().string() + "\n\n";
        out += "```\n" + content;
        if (!content.empty() && content.back() != '\n') out += "\n";
        out += "```\n\n";
    }
    return out;
}

// Knower role names whose vaults carry the AUTHORITATIVE distilled knowledge
// (by lens). The librarian's Pitch / Roadmap lanes derive from these vaults'
// ref-*.md content, NOT from learnings.md. Discovered by walking
// .quorum/vaults/* and matching these names; if none match we fall back to
// "all vaults except scribe" (the scribe journal feeds the Decision Log only).
[[nodiscard]] inline bool is_known_knower(const std::string& name) {
    return name == "cartographer" || name == "architect" ||
           name == "historian" || name == "recap";
}

// Extract a ref note's frontmatter `summary:` line (the write-for-retrieval
// preview, Decision #44), if present in the leading `--- ... ---` block. Empty
// string when absent. Used to prefer the one-line summary over a raw head
// excerpt so the digest stays compact and on-topic.
[[nodiscard]] inline std::string frontmatter_summary(const std::string& content) {
    if (content.rfind("---", 0) != 0) return {};
    // Find the closing fence of the frontmatter block.
    size_t close = content.find("\n---", 3);
    if (close == std::string::npos) return {};
    std::string fm = content.substr(0, close);
    std::istringstream iss(fm);
    std::string line;
    while (std::getline(iss, line)) {
        // Trim leading whitespace.
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        std::string trimmed = line.substr(s);
        const std::string key = "summary:";
        if (trimmed.rfind(key, 0) == 0) {
            std::string val = trimmed.substr(key.size());
            size_t vs = val.find_first_not_of(" \t");
            if (vs == std::string::npos) return {};
            val = val.substr(vs);
            // Strip a trailing CR if any.
            if (!val.empty() && val.back() == '\r') val.pop_back();
            return val;
        }
    }
    return {};
}

// Build a digest of the KNOWER vaults — the AUTHORITATIVE distilled knowledge
// the Pitch / Roadmap lanes derive from. Mirrors scribe_vault_digest's shape
// (filenames + head of the most-recent notes, same caps) but:
//   - enumerates ONLY ref-*.md (the durable, retrieval-shaped notes), skipping
//     rule-*.md (operating rules, not pitch material);
//   - aggregates across every knower vault under <project_root>/.quorum/vaults/
//     selected by is_known_knower (fallback: all vaults except `scribe`);
//   - also folds in <project_root>/.quorum/knowledge/ref-*.md (project-scope
//     promoted refs — vault_manager auto-promotion target);
//   - prefers each ref's frontmatter `summary:` line as the excerpt when present.
// Plain filesystem only (does NOT import context_assembler.h — replicates the
// lightweight sibling-vault walk that helper performs).
[[nodiscard]] inline std::string knower_vault_digest(
    const std::filesystem::path& project_root, size_t max_recent = 5,
    size_t head_chars = 800) {
    namespace fs = std::filesystem;
    std::string out;

    struct Ref {
        fs::path path;
        std::string vault_label;   // "vault: <knower>" or "project knowledge"
        fs::file_time_type mtime;
    };
    std::vector<Ref> refs;

    auto collect_refs = [&](const fs::path& knowledge_dir,
                            const std::string& label) {
        if (!fs::exists(knowledge_dir) || !fs::is_directory(knowledge_dir))
            return;
        for (const auto& e : fs::directory_iterator(knowledge_dir)) {
            if (!e.is_regular_file()) continue;
            const auto fn = e.path().filename().string();
            if (e.path().extension() != ".md") continue;
            if (fn.rfind("ref-", 0) != 0) continue;   // ref-* only; skip rule-*
            std::error_code ec;
            auto mt = fs::last_write_time(e.path(), ec);
            refs.push_back({e.path(), label, mt});
        }
    };

    // 1) Walk sibling vaults under .quorum/vaults/, selecting knowers. Two-pass:
    //    prefer known knower roles; if none matched, fall back to all-but-scribe.
    auto vaults_root = project_root / ".quorum" / "vaults";
    std::vector<std::pair<fs::path, std::string>> selected;  // (knowledge_dir, label)
    std::vector<std::pair<fs::path, std::string>> non_scribe; // fallback pool
    if (fs::exists(vaults_root) && fs::is_directory(vaults_root)) {
        for (const auto& sib : fs::directory_iterator(vaults_root)) {
            if (!sib.is_directory()) continue;
            auto name = sib.path().filename().string();
            auto knowledge = sib.path() / "knowledge";
            if (!fs::exists(knowledge) || !fs::is_directory(knowledge)) continue;
            std::string label = "vault: " + name;
            if (is_known_knower(name)) {
                selected.push_back({knowledge, label});
            }
            if (name != "scribe") {
                non_scribe.push_back({knowledge, label});
            }
        }
    }
    const auto& knower_dirs = selected.empty() ? non_scribe : selected;
    for (const auto& [dir, label] : knower_dirs) {
        collect_refs(dir, label);
    }

    // 2) Project-scope promoted refs (vault_manager auto-promotion target).
    collect_refs(project_root / ".quorum" / "knowledge", "project knowledge");

    if (refs.empty()) {
        out += "(no knower-vault ref-*.md notes found — Pitch/Roadmap have no "
               "authoritative source yet)\n";
        return out;
    }

    // Filenames first (deterministic order), labelled by source vault.
    std::sort(refs.begin(), refs.end(), [](const Ref& a, const Ref& b) {
        return a.path.filename().string() < b.path.filename().string();
    });
    out += "Knower ref notes (authoritative — by lens):\n";
    for (const auto& r : refs) {
        out += "- " + r.path.filename().string() + "  [" + r.vault_label + "]\n";
    }
    out += "\n";

    // Most-recent refs (by mtime), bounded head excerpt; prefer `summary:`.
    std::sort(refs.begin(), refs.end(),
        [](const Ref& a, const Ref& b) { return a.mtime > b.mtime; });
    size_t shown = std::min(max_recent, refs.size());
    out += "Recent refs (summary or head):\n\n";
    for (size_t i = 0; i < shown; ++i) {
        auto content = slurp(refs[i].path);
        auto summary = frontmatter_summary(content);
        out += "#### " + refs[i].path.filename().string() +
               "  [" + refs[i].vault_label + "]\n\n";
        if (!summary.empty()) {
            out += "summary: " + summary + "\n\n";
        } else {
            if (content.size() > head_chars) {
                content = content.substr(0, head_chars) + "\n... (truncated)";
            }
            out += "```\n" + content;
            if (!content.empty() && content.back() != '\n') out += "\n";
            out += "```\n\n";
        }
    }
    return out;
}

}  // namespace detail

// Compose the curation prompt fed to the librarian agent. Per pitch-protocol.md
// "Curation prompt" (v0.4):
//   (1) current contents of the four output files so the librarian proposes
//       deltas, not restatements;
//   (2) the KNOWER-VAULT digest — the AUTHORITATIVE distilled knowledge (by
//       lens) that feeds the Pitch + Roadmap lanes; plus .quorum/learnings.md,
//       which feeds the Decision Log lane only;
//   (3) instructions covering the block formats, the source-routing table, and
//       the "cite source, propose deltas only, do not invent" rules.
//
// Source-of-truth linearization (v0.4): scribe journal -> knower vaults
// (authoritative) -> librarian projection (human view). Pitch/Roadmap derive
// from knower ref-*.md; the chronological Decision Log still draws from the
// journal's `decisions` field.
[[nodiscard]] inline std::string assemble_curation_prompt(
    const std::string& project_root) {
    namespace fs = std::filesystem;
    fs::path root(project_root);

    std::string p;
    p += "You are the Quorum librarian, running as a periodic CURATOR "
         "(analyst-class, read-only).\n\n";
    p += "Your job: PROJECT this project's authoritative knowledge into its "
         "human-facing aspirational layer (Pitch / Decision Log / Roadmap, "
         "stored under .quorum/librarian/). You do NOT write files. You emit "
         "structured blocks; the daemon applies them behind an "
         "operator-approval diff gate.\n\n";
    p += "Source of truth (linearized): scribe journal -> KNOWER VAULTS "
         "(authoritative, by lens) -> librarian projection (this layer). The "
         "Pitch and Roadmap lanes derive from the KNOWER VAULTS' ref notes; the "
         "chronological Decision Log derives from the scribe journal "
         "(learnings.md `decisions`). Do NOT re-derive Pitch/Roadmap content "
         "from learnings.md — the knower vaults are the authoritative home.\n\n";

    p += "## Current aspirational layer (propose DELTAS against this — Rule 6 "
         "idempotency)\n\n";
    auto curated = sui::quorum::detail::curated_base(project_root);
    detail::append_file_section(p, "Pitch / Introduction",
                                curated / "Pitch" / "00 - Introduction.md");
    detail::append_file_section(p, "Pitch / Anti-goals",
                                curated / "Pitch" / "01 - Anti-goals.md");
    detail::append_file_section(p, "Decision Log",
                                curated / "00 - Decision Log.md");
    detail::append_file_section(p, "Roadmap",
                                curated / "01 - Roadmap.md");

    p += "## Authoritative source for Pitch + Roadmap — KNOWER VAULTS\n\n";
    p += "These ref-*.md notes (cartographer / architect / historian / recap, "
         "plus project-promoted refs) are the AUTHORITATIVE distilled knowledge. "
         "Derive Pitch (What we're building / Why it matters / Current "
         "direction) and Roadmap (Open items) ONLY from this digest.\n\n";
    p += detail::knower_vault_digest(root);
    p += "\n";

    p += "## Source for Decision Log — scribe journal\n\n";
    p += "### .quorum/learnings.md\n\n";
    {
        auto learnings = detail::slurp(root / ".quorum" / "learnings.md");
        if (learnings.empty()) {
            p += "(no learnings.md yet)\n\n";
        } else {
            p += "```\n" + learnings;
            if (learnings.back() != '\n') p += "\n";
            p += "```\n\n";
        }
    }
    // Scribe-vault digest retained as SECONDARY context only (not a Pitch/Roadmap
    // source): it can help phrase Decision Log entries that reference a scribe
    // note, but it must NOT seed Pitch/Roadmap proposals — those come from the
    // knower vaults above.
    p += "### Scribe knowledge vault digest (secondary context — Decision Log "
         "phrasing only)\n\n";
    p += detail::scribe_vault_digest(
             root / ".quorum" / "vaults" / "scribe" / "knowledge");
    p += "\n";

    p += "## Source-routing table (which input feeds which output lane)\n\n";
    p += "| source | output file | block | semantics |\n";
    p += "|---|---|---|---|\n";
    p += "| knower vaults (ref-*) | Pitch/00 - Introduction.md | CURATION_UPDATE | "
         "section: What we're building / Why it matters / Current direction |\n";
    p += "| knower vaults (ref-*) | 01 - Roadmap.md            | CURATION_UPDATE | "
         "section: Open items |\n";
    p += "| knower vaults (ref-*) | Pitch/01 - Anti-goals.md   | CURATION_UPDATE | "
         "section: Anti-goals (explicit non-goals named in the refs) |\n";
    p += "| learnings.md `decisions` | 00 - Decision Log.md     | DECISION_LOG_APPEND | "
         "one append per decision (chronological) |\n\n";

    p += "## Block formats (emit ONLY these; the daemon applies them)\n\n";
    p += "```CURATION_UPDATE\n";
    p += "file: Pitch/00 - Introduction.md\n";
    p += "section: Current direction\n";
    p += "content: |\n";
    p += "  - {distilled bullet}\n";
    p += "source: vault: <knower> ref-<slug>.md\n";
    p += "```\n\n";
    p += "Valid CURATION_UPDATE targets (file :: section):\n";
    p += "- Pitch/00 - Introduction.md :: What we're building | Why it matters "
         "| Current direction\n";
    p += "- Pitch/01 - Anti-goals.md :: Anti-goals\n";
    p += "- 01 - Roadmap.md :: Open items\n\n";
    p += "```DECISION_LOG_APPEND\n";
    p += "utc: 2026-05-29T09:10:00Z\n";
    p += "decision: |\n";
    p += "  {decision statement}\n";
    p += "rationale: |\n";
    p += "  {why}\n";
    p += "source: learnings.md {utc}\n";
    p += "```\n\n";

    p += "## Rules\n\n";
    p += "1. Propose ONLY changes not already reflected in the current files "
         "(deltas — Rule 6). If the knower vaults are unchanged since the last "
         "curate, emit NO Pitch/Roadmap proposals.\n";
    p += "2. Pitch (What we're building / Why it matters / Current direction) "
         "and Roadmap (Open items) CURATION_UPDATE blocks derive from the KNOWER "
         "VAULTS' ref notes — cite the ref note in `source:` (e.g. "
         "`vault: architect ref-design.md`). DECISION_LOG_APPEND blocks derive "
         "from learnings.md `decisions` — cite `learnings.md {utc}`. Do NOT "
         "invent claims absent from those sources (Rule 7).\n";
    p += "3. CURATION_UPDATE replaces the named section's body; pick canonical "
         "sections only — unknown sections are dropped.\n";
    p += "4. Route per the source-routing table; do not cross lanes. In "
         "particular, do NOT seed Pitch/Roadmap from learnings.md — the knower "
         "vaults are their authoritative source.\n";
    p += "5. Emit no file writes. The daemon applies your blocks behind an "
         "operator gate.\n";
    return p;
}

// Run the parse -> validate -> diff -> apply pipeline over the librarian's raw
// output. PURE w.r.t. claude (no invocation). Returns a CurationPlan listing
// every proposal with its diff and status.
//
// In PreviewOnly / Interactive mode the pipeline computes the diff WITHOUT
// writing (status "previewed"). In ApplyAll mode it applies each valid proposal
// via the Batch A primitives (status "applied"); invalid proposals are
// "rejected" with a reason and the rest still apply.
[[nodiscard]] inline CurationPlan run_curation_pipeline(
    const std::string& project_root, const std::string& raw_librarian_output,
    ApplyMode mode) {
    CurationPlan plan;
    OutputParser parser;
    auto parsed = parser.parse(raw_librarian_output);

    // --- CURATION_UPDATE proposals --------------------------------------------
    for (const auto& cu : parsed.curation_updates) {
        CurationProposal prop;
        prop.kind = CurationProposal::Kind::CurationUpdate;
        prop.file = cu.file;
        prop.target = cu.file + " :: ## " + cu.section;
        prop.curation_update = cu;

        // Validate target up front so PreviewOnly can report rejection too.
        if (!sui::quorum::detail::is_canonical_file(cu.file)) {
            prop.status = "rejected";
            prop.reason = "'" + cu.file +
                          "' is not a canonical curation output file";
            plan.proposals.push_back(std::move(prop));
            continue;
        }
        if (!sui::quorum::detail::is_curatable_section(cu.file, cu.section)) {
            prop.status = "rejected";
            prop.reason = "section '" + cu.section +
                          "' is not curatable for '" + cu.file + "'";
            plan.proposals.push_back(std::move(prop));
            continue;
        }

        if (mode == ApplyMode::ApplyAll) {
            auto r = sui::quorum::apply_curation_update(project_root, cu);
            // Check skipped FIRST: an operator-owned section returns ok=true +
            // skipped=true (deliberately NOT written), distinct from applied.
            if (r.skipped) {
                prop.status = "skipped";
                prop.reason = r.reason;
            } else if (r.ok) {
                prop.status = "applied";
                prop.diff = r.diff;
            } else {
                prop.status = "rejected";
                prop.reason = r.reason;
            }
        } else {
            // PreviewOnly / Interactive: render the diff against the current
            // file body WITHOUT writing. Bootstrap a missing file's skeleton in
            // memory only.
            std::filesystem::path target =
                sui::quorum::detail::curated_base(project_root) / cu.file;
            std::string content;
            std::error_code ec;
            if (std::filesystem::exists(target, ec)) {
                content = sui::quorum::detail::read_file_text(target);
            } else {
                content = sui::quorum::detail::skeleton_for(cu.file);
            }
            std::string rewritten, old_body;
            if (sui::quorum::detail::replace_section_body(
                    content, cu.section, cu.content, rewritten, old_body)) {
                // Operator-owned section lock: mirror the apply-time guard so the
                // preview reports a skip the apply would honour, not a phantom diff.
                if (old_body.find(sui::quorum::detail::kOperatorOwnedMarker) !=
                    std::string::npos) {
                    prop.status = "skipped";
                    prop.reason = "section is operator-owned (" +
                                  sui::quorum::detail::kOperatorOwnedMarker +
                                  " marker) — would not be overwritten";
                } else {
                    prop.diff = sui::quorum::detail::render_section_diff(
                        cu.file, cu.section, old_body, cu.content);
                    prop.status = "previewed";
                }
            } else {
                prop.status = "rejected";
                prop.reason = "section heading '## " + cu.section +
                              "' not found in '" + cu.file + "'";
            }
        }
        plan.proposals.push_back(std::move(prop));
    }

    // --- DECISION_LOG_APPEND proposals ----------------------------------------
    for (const auto& dla : parsed.decision_log_appends) {
        CurationProposal prop;
        prop.kind = CurationProposal::Kind::DecisionLogAppend;
        prop.file = sui::quorum::detail::kDecisionLog;
        prop.decision_append = dla;
        std::string title = sui::quorum::detail::first_line(dla.decision);
        prop.target = prop.file + " :: " + title;

        // Validate required fields (mirror the primitive's gate) so preview
        // reports the same rejections the apply path would.
        if (dla.utc.empty()) {
            prop.status = "rejected";
            prop.reason = "DECISION_LOG_APPEND utc is empty";
            plan.proposals.push_back(std::move(prop));
            continue;
        }
        if (title.empty()) {
            prop.status = "rejected";
            prop.reason = "DECISION_LOG_APPEND decision is empty";
            plan.proposals.push_back(std::move(prop));
            continue;
        }

        if (mode == ApplyMode::ApplyAll) {
            auto r = sui::quorum::apply_decision_log_append(project_root, dla);
            if (r.ok) {
                prop.status = "applied";
                prop.diff = r.diff;
            } else {
                prop.status = "rejected";
                prop.reason = r.reason;
            }
        } else {
            std::string date = sui::quorum::detail::date_from_utc(dla.utc);
            prop.diff = "+ ### " + date + " \xe2\x80\x94 " + title + "\n";
            prop.status = "previewed";
        }
        plan.proposals.push_back(std::move(prop));
    }

    return plan;
}

namespace detail {

// Render one proposal block (label, status, diff) to stdout.
inline void print_proposal(std::ostream& os, size_t idx,
                           const CurationProposal& p) {
    const char* kind = (p.kind == CurationProposal::Kind::CurationUpdate)
                           ? "CURATION_UPDATE"
                           : "DECISION_LOG_APPEND";
    os << "[" << idx << "] " << kind << " -> " << p.target << "\n";
    if (!p.diff.empty()) {
        os << p.diff;
        if (p.diff.back() != '\n') os << "\n";
    }
    if (!p.reason.empty()) {
        os << "    (" << p.status << ": " << p.reason << ")\n";
    }
    os << "\n";
}

// Per-proposal y/n approve prompt for Interactive mode. Default (Enter) = no.
[[nodiscard]] inline bool prompt_approve(const CurationProposal& p) {
    std::cout << "Apply this proposal? [y/N]: " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return false;
    for (char c : line) {
        if (c == ' ' || c == '\t') continue;
        return (c == 'y' || c == 'Y');
    }
    return false;
}

}  // namespace detail

// Top-level entrypoint for `quorum librarian curate`. Orchestrates:
//   1. resolve project_path (default cwd)
//   2. ensure_curation_skeleton (print what was bootstrapped)
//   3. assemble_curation_prompt
//   4. live `claude -p` invoke (analyst-clamped, agent_create.h pattern)
//   5. run_curation_pipeline
//   6. render diffs + operator gate (dry_run / apply_all / interactive)
//   7. print summary
//
// Return 0 on success; 1 on a hard error (claude invocation failure, etc.).
[[nodiscard]] inline int run_librarian_curate(const LibrarianCurateOptions& opts) {
    namespace fs = std::filesystem;

    // 1. Resolve project root.
    std::string project_root = opts.project_path;
    if (project_root.empty()) {
        project_root = fs::current_path().string();
    }

    // 2. Bootstrap the skeleton (Rule 5: cooperate, never overwrite).
    auto skel = sui::quorum::ensure_curation_skeleton(project_root);
    if (!skel.ok) {
        std::cerr << "ERROR: " << skel.reason << "\n";
        return 1;
    }
    if (skel.bootstrapped) {
        std::cout << "Bootstrapped missing curation output files under "
                  << project_root << "\n";
    }

    // 3. Compose the prompt.
    auto prompt = assemble_curation_prompt(project_root);

    // 4. Live claude -p invocation — analyst-clamped (read-only). The librarian
    //    CANNOT write files; the daemon applies its blocks below. Modelled on
    //    cli/agent_create.h's lightweight synchronous pattern.
    auto temp_path = "/tmp/quorum_librarian_curate_" +
                     std::to_string(::getpid()) + ".txt";
    {
        std::ofstream f(temp_path, std::ios::trunc);
        f << prompt;
    }
    std::string model_flag = opts.model.empty() ? "" : " --model " + opts.model;
    std::cout << "Invoking librarian (claude -p, analyst/read-only"
              << (opts.model.empty() ? "" : ", model=" + opts.model) << ")...\n";
    auto cmd = "env -u CLAUDECODE cat " + temp_path +
               " | claude -p --dangerously-skip-permissions" + model_flag +
               " --disallowedTools \"Write,Edit,NotebookEdit\"" +
               " --output-format json 2>&1";
    auto result = sui::quorum::run_command(cmd);
    std::remove(temp_path.c_str());

    if (!result || result->exit_code != 0) {
        std::cerr << "ERROR: claude -p invocation failed";
        if (result) std::cerr << " (exit " << result->exit_code << ")";
        std::cerr << "\n";
        return 1;
    }
    auto text = sui::quorum::json::extract_string(result->output, "result");
    if (!text || text->empty()) {
        std::cerr << "ERROR: librarian produced no output\n";
        return 1;
    }
    std::string raw = *text;

    // 5. Run the pipeline. dry_run / interactive preview first; apply_all writes.
    ApplyMode mode = opts.dry_run      ? ApplyMode::PreviewOnly
                     : opts.apply_all  ? ApplyMode::ApplyAll
                                       : ApplyMode::Interactive;
    auto plan = run_curation_pipeline(project_root, raw, mode);

    if (plan.proposals.empty()) {
        std::cout << "No curation proposals (nothing to update).\n";
        return 0;
    }

    // 6. Render diffs + operator gate.
    std::cout << "\n=== Proposed curation (" << plan.proposals.size()
              << " proposal(s)) ===\n\n";
    for (size_t i = 0; i < plan.proposals.size(); ++i) {
        detail::print_proposal(std::cout, i + 1, plan.proposals[i]);
    }

    if (opts.dry_run) {
        std::cout << "--dry-run: no files written.\n";
        return 0;
    }

    if (opts.apply_all) {
        // ApplyAll already wrote during the pipeline.
        // Surface operator-owned skips explicitly: those sections were locked and
        // deliberately NOT overwritten.
        for (const auto& p : plan.proposals) {
            if (p.status == "skipped") {
                std::cout << "[" << plan.skipped_count()
                          << "] skipped (operator-owned) -> " << p.target << "\n";
            }
        }
        std::cout << "Summary: " << plan.proposals.size() << " proposal(s), "
                  << plan.applied_count() << " applied, "
                  << plan.skipped_count() << " skipped (operator-owned), "
                  << plan.rejected_count() << " rejected.\n";
        return 0;
    }

    // Interactive: per-proposal approve, then apply approved ones.
    size_t applied = 0, skipped = 0, rejected = 0;
    for (auto& p : plan.proposals) {
        if (p.status == "rejected") {
            ++rejected;
            continue;
        }
        // Operator-owned sections are locked by the pipeline (status "skipped"):
        // never prompt, never apply — surface and move on.
        if (p.status == "skipped") {
            ++skipped;
            std::cout << "[" << p.target << "]\n"
                      << "  -> skipped (operator-owned)\n";
            continue;
        }
        std::cout << "[" << p.target << "]\n";
        if (!detail::prompt_approve(p)) {
            p.status = "skipped";
            ++skipped;
            std::cout << "  -> skipped\n";
            continue;
        }
        CurationResult r;
        if (p.kind == CurationProposal::Kind::CurationUpdate) {
            r = sui::quorum::apply_curation_update(project_root,
                                                   p.curation_update);
        } else {
            r = sui::quorum::apply_decision_log_append(project_root,
                                                       p.decision_append);
        }
        if (r.skipped) {
            // Section became operator-owned between preview and apply — honour
            // the lock even after operator approval.
            p.status = "skipped";
            p.reason = r.reason;
            ++skipped;
            std::cout << "  -> skipped (operator-owned)\n";
        } else if (r.ok) {
            p.status = "applied";
            ++applied;
            std::cout << "  -> applied\n";
        } else {
            p.status = "rejected";
            p.reason = r.reason;
            ++rejected;
            std::cout << "  -> rejected: " << r.reason << "\n";
        }
    }

    // 7. Summary.
    std::cout << "\nSummary: " << plan.proposals.size() << " proposal(s), "
              << applied << " applied, " << skipped << " skipped, "
              << rejected << " rejected.\n";
    return 0;
}

}  // namespace sui::quorum::cli
