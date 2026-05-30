#pragma once

// Phase 11 - Librarian as Curator. C++ write/parse engine for the curation
// cycle defined in templates/specs/pitch-protocol.md (v0.1).
//
// The librarian runs ANALYST-CLASS at runtime: the daemon clamps Write/Edit
// for every non-`doer` role (invoker.h::build_tool_flags). So the librarian
// never writes files itself - it emits CURATION_UPDATE / DECISION_LOG_APPEND
// blocks and the daemon performs all writes through the primitives below,
// behind an operator-approval diff gate. This mirrors the scribe path:
//   LEARNINGS_UPDATE block -> apply_scribe_learnings_update().
//
// The four curation output files live UNDER <project_root>/.quorum/librarian/,
// keeping the whole curated layer self-contained inside .quorum/ (mirroring the
// knower dirs like .quorum/historian/). They are human-facing project docs, NOT
// agent-loop audit files:
//
//   <project_root>/.quorum/librarian/
//   ├── Pitch/
//   │   ├── 00 - Introduction.md
//   │   └── 01 - Anti-goals.md
//   ├── 00 - Decision Log.md
//   └── 01 - Roadmap.md
//
// The CURATION_UPDATE / DECISION_LOG_APPEND block `file:` fields stay RELATIVE
// (e.g. "Pitch/00 - Introduction.md", "00 - Decision Log.md") — only the on-disk
// base directory moved. See detail::curated_base().
//
// Header-only, matches the vault/scribe_writer.h convention. Reuses
// scribe_writer.h's detail::atomic_write_text + detail::read_file_text rather
// than duplicating the atomic/fsync discipline.

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "vault/scribe_writer.h"

namespace sui::quorum {

// One section-scoped curation proposal. `file` and `section` MUST be canonical
// (see detail::is_curatable_section); `content` is the replacement body for the
// named section. `source` is optional provenance.
struct CurationUpdate {
    std::string file;     // e.g. "Pitch/00 - Introduction.md"
    std::string section;  // canonical heading text, WITHOUT leading "## "
    std::string content;  // replacement body for that section
    std::string source;   // optional provenance citation
};

// One append-only Decision Log entry. `utc` + `decision` are required.
struct DecisionLogAppend {
    std::string utc;        // "2026-05-29T09:10:00Z"
    std::string decision;   // decision statement (first line becomes the title)
    std::string rationale;  // optional "why"
    std::string source;     // optional provenance citation
};

// Outcome of a curation primitive call. On rejection, `reason` carries a
// human-readable diagnostic suitable for stderr logging. `diff` is a simple
// unified-ish before/after of the changed region (consumed by the CLI later).
struct CurationResult {
    bool ok{false};
    std::string reason;
    bool bootstrapped{false};
    std::string diff;
    bool skipped{false};  // section was operator-owned (marker present) — deliberately NOT written
};

namespace detail {

// The four canonical output file paths, relative to the curated base (NOT the
// project root). These stay the block `file:` identifiers; only their on-disk
// base directory moved — see curated_base().
inline const std::string kPitchIntro    = "Pitch/00 - Introduction.md";
inline const std::string kPitchAntiGoals = "Pitch/01 - Anti-goals.md";
inline const std::string kDecisionLog   = "00 - Decision Log.md";
inline const std::string kRoadmap       = "01 - Roadmap.md";

// Operator-owned section lock. If a curated section's CURRENT body contains this
// literal HTML-comment marker, CURATION_UPDATE never overwrites it — the
// operator's hard, section-scoped lock. Enforced at apply time in the write
// primitive (apply_curation_update), so it protects both the CLI and the daemon
// paths. DECISION_LOG_APPEND is append-only and never overwrites, so it is
// exempt. The marker has no effect on first creation (bootstrap skeleton bodies
// never contain it).
inline const std::string kOperatorOwnedMarker = "<!-- operator-owned -->";

// The curated layer lives under <project_root>/.quorum/librarian/ (self-contained
// in .quorum/, like the knower dirs). The CURATION_UPDATE/DECISION_LOG_APPEND
// block `file:` fields stay relative (e.g. "Pitch/00 - Introduction.md"); they
// resolve under this base.
inline std::filesystem::path curated_base(std::string_view project_root) {
    return std::filesystem::path(project_root) / ".quorum" / "librarian";
}

inline const std::vector<std::string>& canonical_files() {
    static const std::vector<std::string> kFiles{
        kPitchIntro, kPitchAntiGoals, kDecisionLog, kRoadmap,
    };
    return kFiles;
}

inline bool is_canonical_file(std::string_view f) {
    const auto& files = canonical_files();
    return std::find(files.begin(), files.end(), f) != files.end();
}

// Curatable (CURATION_UPDATE-targetable) sections per file, per spec v0.1
// "Canonical schema for output files". The Decision Log is append-only and is
// NOT a CURATION_UPDATE target (returns false for any section).
inline bool is_curatable_section(std::string_view file, std::string_view section) {
    if (file == kPitchIntro) {
        return section == "What we're building" ||
               section == "Why it matters" ||
               section == "Current direction";
    }
    if (file == kPitchAntiGoals) {
        return section == "Anti-goals";
    }
    if (file == kRoadmap) {
        return section == "Open items";
    }
    // kDecisionLog and any non-canonical file: not a CURATION_UPDATE target.
    return false;
}

// Today's date (YYYY-MM-DD), used only for the {date} placeholder in freshly
// bootstrapped skeleton frontmatter / headings. Derived from utc strings for
// Decision Log entries, so this is the bootstrap-only fallback.
inline std::string today_date() {
    std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday);
    return std::string(buf);
}

// Derive the YYYY-MM-DD date prefix of a UTC ISO-8601 string. If the input is
// too short to contain a date prefix, return it unchanged (caller has already
// gated on non-empty utc).
inline std::string date_from_utc(std::string_view utc) {
    if (utc.size() >= 10) return std::string(utc.substr(0, 10));
    return std::string(utc);
}

// First line of a (possibly multi-line) string, trimmed of trailing CR/LF and
// surrounding spaces. Used for the Decision Log entry title.
inline std::string first_line(std::string_view s) {
    auto nl = s.find('\n');
    std::string_view line = (nl == std::string_view::npos) ? s : s.substr(0, nl);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t' ||
                             line.front() == '\r'))
        line.remove_prefix(1);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
                             line.back() == '\r'))
        line.remove_suffix(1);
    return std::string(line);
}

// Canonical skeleton bodies, verbatim from spec v0.1 "Canonical schema for
// output files". {date} is filled with today's date at bootstrap time.

inline std::string render_pitch_intro_skeleton() {
    std::string d = today_date();
    std::string out;
    out += "---\n";
    out += "title: Pitch\n";
    out += "updated: " + d + "\n";
    out += "---\n\n";
    out += "# Pitch\n\n";
    out += "## What we're building\n\n";
    out += "## Why it matters\n\n";
    out += "## Current direction\n\n";
    out += "## What we're NOT doing\n\n";
    out += "See [[01 - Anti-goals]].\n";
    return out;
}

inline std::string render_pitch_antigoals_skeleton() {
    std::string d = today_date();
    std::string out;
    out += "---\n";
    out += "title: Anti-goals\n";
    out += "updated: " + d + "\n";
    out += "---\n\n";
    out += "# Anti-goals\n\n";
    out += "## Anti-goals\n";
    return out;
}

inline std::string render_decision_log_skeleton() {
    std::string d = today_date();
    std::string out;
    out += "---\n";
    out += "title: Decision Log\n";
    out += "updated: " + d + "\n";
    out += "---\n\n";
    out += "# Decision Log\n\n";
    out += "Append-only. One entry per scribe-flagged decision.\n";
    return out;
}

inline std::string render_roadmap_skeleton() {
    std::string d = today_date();
    std::string out;
    out += "---\n";
    out += "title: Roadmap\n";
    out += "updated: " + d + "\n";
    out += "---\n\n";
    out += "# Roadmap\n\n";
    out += "## Open items\n";
    return out;
}

// Map a canonical file path to its skeleton body. Returns empty string for a
// non-canonical file (caller must have validated first).
inline std::string skeleton_for(std::string_view file) {
    if (file == kPitchIntro)     return render_pitch_intro_skeleton();
    if (file == kPitchAntiGoals) return render_pitch_antigoals_skeleton();
    if (file == kDecisionLog)    return render_decision_log_skeleton();
    if (file == kRoadmap)        return render_roadmap_skeleton();
    return {};
}

// Replace the body of the section headed `## {section}` in `content` with
// `new_body`. The heading line itself is preserved; everything from after the
// heading line up to (but not including) the next line starting with "## " or
// EOF is replaced. The new body is wrapped with one blank line on each side so
// the result keeps canonical "## X\n\n<body>\n\n## Y" spacing.
//
// Returns true and writes the rewritten content into `out` on success. Returns
// false (and leaves `out` untouched) if the `## {section}` heading is absent.
// `old_body` receives the prior section body (for diff rendering).
inline bool replace_section_body(const std::string& content,
                                 std::string_view section,
                                 const std::string& new_body,
                                 std::string& out,
                                 std::string& old_body) {
    const std::string heading = "## " + std::string(section);

    // Find the heading at line-start.
    size_t hpos = std::string::npos;
    size_t scan = 0;
    while (scan <= content.size()) {
        size_t found = content.find(heading, scan);
        if (found == std::string::npos) break;
        bool at_line_start = (found == 0) || (content[found - 1] == '\n');
        // Ensure the heading is the whole heading token: next char is end,
        // newline, or whitespace (so "## Open items" doesn't match
        // "## Open items extended").
        size_t after = found + heading.size();
        bool clean_end = (after == content.size()) ||
                         content[after] == '\n' || content[after] == '\r' ||
                         content[after] == ' ' || content[after] == '\t';
        if (at_line_start && clean_end) { hpos = found; break; }
        scan = found + heading.size();
    }
    if (hpos == std::string::npos) return false;

    // End of the heading line.
    size_t heading_eol = content.find('\n', hpos);
    size_t body_start;
    if (heading_eol == std::string::npos) {
        // Heading at EOF with no trailing newline; body is empty.
        body_start = content.size();
    } else {
        body_start = heading_eol + 1;
    }

    // Find the next "## " heading at line-start (the section boundary).
    size_t body_end = content.size();
    size_t cursor = body_start;
    while (cursor < content.size()) {
        size_t nl = content.find("## ", cursor);
        if (nl == std::string::npos) break;
        bool at_line_start = (nl == 0) || (content[nl - 1] == '\n');
        if (at_line_start) { body_end = nl; break; }
        cursor = nl + 3;
    }

    old_body = content.substr(body_start, body_end - body_start);

    // Trim the replacement body's surrounding blank lines, then re-wrap to
    // canonical "\n<body>\n\n" so the heading is followed by one blank line and
    // the next heading is preceded by one blank line.
    std::string body = new_body;
    // Strip leading newlines.
    size_t b0 = 0;
    while (b0 < body.size() && (body[b0] == '\n' || body[b0] == '\r')) ++b0;
    // Strip trailing newlines/spaces.
    size_t b1 = body.size();
    while (b1 > b0 && (body[b1 - 1] == '\n' || body[b1 - 1] == '\r' ||
                       body[b1 - 1] == ' ' || body[b1 - 1] == '\t'))
        --b1;
    std::string trimmed = body.substr(b0, b1 - b0);

    std::string replacement;
    replacement += "\n";  // blank line after the heading
    if (!trimmed.empty()) {
        replacement += trimmed;
        replacement += "\n";
    }
    // One trailing blank line before the next section (or EOF). Only add the
    // separating blank line if there is a following section.
    if (body_end < content.size()) {
        replacement += "\n";
    }

    out.clear();
    out.reserve(content.size() + replacement.size());
    out.append(content, 0, body_start);
    out.append(replacement);
    out.append(content, body_end, std::string::npos);
    return true;
}

// Render a minimal unified-ish diff of a single section's body change.
inline std::string render_section_diff(std::string_view file,
                                        std::string_view section,
                                        const std::string& old_body,
                                        const std::string& new_body) {
    std::string out;
    out += "--- " + std::string(file) + " :: ## " + std::string(section) + "\n";
    out += "+++ " + std::string(file) + " :: ## " + std::string(section) + "\n";

    auto emit_lines = [&](char prefix, const std::string& body) {
        std::string cur;
        auto flush = [&]() {
            // Strip trailing CR.
            while (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out += prefix;
            out += cur;
            out += '\n';
            cur.clear();
        };
        bool any = false;
        for (char c : body) {
            any = true;
            if (c == '\n') { flush(); }
            else cur += c;
        }
        if (!cur.empty()) flush();
        else if (!any) { /* empty body: emit nothing */ }
    };

    emit_lines('-', old_body);
    emit_lines('+', new_body);
    return out;
}

}  // namespace detail

// Create any MISSING of the four canonical output files with the exact
// canonical structure from spec v0.1. NEVER overwrites an existing file
// (read-exists check first - Rule 5). Creates the Pitch/ subdir as needed.
// Idempotent: a second call on a complete project makes zero changes.
[[nodiscard]] inline CurationResult ensure_curation_skeleton(
    std::string_view project_root) {
    CurationResult result;
    std::filesystem::path root = detail::curated_base(project_root);

    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        result.ok = false;
        result.reason = "librarian_curator: failed to create curated base: " +
                        ec.message();
        return result;
    }

    bool created_any = false;
    for (const auto& rel : detail::canonical_files()) {
        auto target = root / rel;
        std::error_code exists_ec;
        if (std::filesystem::exists(target, exists_ec)) {
            continue;  // never overwrite
        }
        // Ensure parent dir (Pitch/ for the pitch files).
        std::filesystem::create_directories(target.parent_path(), exists_ec);
        std::string err;
        if (!detail::atomic_write_text(target, detail::skeleton_for(rel), err)) {
            result.ok = false;
            result.reason = err;
            return result;
        }
        created_any = true;
    }

    result.ok = true;
    result.bootstrapped = created_any;
    return result;
}

// Replace the body of one named canonical section in one canonical file
// (Rule 3: section-scoped, operator content outside the section preserved).
//
// Validates `file` is one of the four canonical files AND `section` is a valid
// curatable section for that file. Bootstraps the file if missing. Locates the
// `## {section}` heading, replaces its body, preserves the heading, all other
// sections, and frontmatter. Atomic writeback. On invalid file/section or a
// missing heading -> ok=false with a clear reason, NO write.
[[nodiscard]] inline CurationResult apply_curation_update(
    std::string_view project_root, const CurationUpdate& update) {
    CurationResult result;

    if (!detail::is_canonical_file(update.file)) {
        result.ok = false;
        result.reason = "librarian_curator: '" + update.file +
                        "' is not a canonical curation output file";
        return result;
    }
    if (!detail::is_curatable_section(update.file, update.section)) {
        result.ok = false;
        result.reason = "librarian_curator: section '" + update.section +
                        "' is not a curatable section for '" + update.file +
                        "'";
        return result;
    }

    auto target = detail::curated_base(project_root) / update.file;

    std::error_code ec;
    bool exists = std::filesystem::exists(target, ec);
    if (!exists) {
        // Bootstrap the file inline (Rule 5 cooperate-not-gate).
        std::filesystem::create_directories(target.parent_path(), ec);
        std::string err;
        if (!detail::atomic_write_text(target, detail::skeleton_for(update.file),
                                       err)) {
            result.ok = false;
            result.reason = err;
            return result;
        }
        result.bootstrapped = true;
    }

    std::string content = detail::read_file_text(target);
    std::string rewritten;
    std::string old_body;
    if (!detail::replace_section_body(content, update.section, update.content,
                                      rewritten, old_body)) {
        result.ok = false;
        result.reason = "librarian_curator: section heading '## " +
                        update.section + "' not found in '" + update.file +
                        "' (no write)";
        return result;
    }

    // Operator-owned section lock (HARD, apply-time). If the CURRENT section body
    // carries the operator-owned marker, the librarian must NEVER overwrite it.
    // This protects both the CLI and the daemon (both route through this
    // primitive). On the bootstrap-the-file path the body is the skeleton
    // placeholder, which has no marker, so this never fires on first creation.
    if (old_body.find(detail::kOperatorOwnedMarker) != std::string::npos) {
        result.ok = true;
        result.skipped = true;
        result.reason = "section '## " + update.section + "' in '" + update.file +
                        "' is operator-owned (" + detail::kOperatorOwnedMarker +
                        " marker present) — not overwritten";
        return result;  // NO write
    }

    std::string err;
    if (!detail::atomic_write_text(target, rewritten, err)) {
        result.ok = false;
        result.reason = err;
        return result;
    }

    result.ok = true;
    result.diff = detail::render_section_diff(update.file, update.section,
                                              old_body, update.content);
    return result;
}

// Append a timestamped entry to 00 - Decision Log.md (Rule 4: append-only).
// Requires non-empty utc and decision (mirrors the LEARNINGS_UPDATE utc gate);
// otherwise rejected with ok=false and NO write. Bootstraps the file if
// missing. Never rewrites or reorders prior entries. Atomic writeback.
[[nodiscard]] inline CurationResult apply_decision_log_append(
    std::string_view project_root, const DecisionLogAppend& entry) {
    CurationResult result;

    if (entry.utc.empty()) {
        result.ok = false;
        result.reason = "librarian_curator: DECISION_LOG_APPEND utc is empty";
        return result;
    }
    if (detail::first_line(entry.decision).empty()) {
        result.ok = false;
        result.reason = "librarian_curator: DECISION_LOG_APPEND decision is empty";
        return result;
    }

    auto target = detail::curated_base(project_root) / detail::kDecisionLog;

    std::error_code ec;
    bool exists = std::filesystem::exists(target, ec);
    std::string content;
    if (!exists) {
        content = detail::render_decision_log_skeleton();
        std::filesystem::create_directories(target.parent_path(), ec);
        result.bootstrapped = true;
    } else {
        content = detail::read_file_text(target);
    }

    // Build the new entry.
    std::string date = detail::date_from_utc(entry.utc);
    std::string title = detail::first_line(entry.decision);
    std::string why = entry.rationale.empty()
                          ? std::string{}
                          : detail::first_line(entry.rationale);
    // Rationale may be multi-line; preserve it whole (trim trailing newlines).
    std::string rationale = entry.rationale;
    while (!rationale.empty() &&
           (rationale.back() == '\n' || rationale.back() == '\r' ||
            rationale.back() == ' '))
        rationale.pop_back();
    std::string source = entry.source.empty()
                             ? ("learnings.md " + entry.utc)
                             : entry.source;

    std::string appended;
    appended += "### " + date + " \xe2\x80\x94 " + title + "\n\n";
    appended += "**Why:** " + rationale + "\n\n";
    appended += "**Source:** " + source + "\n";

    // Ensure exactly one blank line between prior content and the new entry.
    if (!content.empty() && content.back() != '\n') content += '\n';
    while (content.size() >= 2 && content[content.size() - 1] == '\n' &&
           content[content.size() - 2] == '\n')
        content.pop_back();
    content += "\n";  // single blank-line separator
    content += appended;

    std::string err;
    if (!detail::atomic_write_text(target, content, err)) {
        result.ok = false;
        result.reason = err;
        return result;
    }

    result.ok = true;
    result.diff = "+ " + std::string("### ") + date + " \xe2\x80\x94 " +
                  title + "\n";
    return result;
}

}  // namespace sui::quorum
