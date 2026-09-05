#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "storage/schema.h"
#include "cli/agent_create.h"

namespace fs = std::filesystem;
namespace sui::quorum::cli {

// A language "specialty" doer that init can auto-attach to a fresh project
// based on a marker file (F10). `marker` records WHICH file triggered detection,
// relative to the project root ("Move.toml" or "quorum-core/CMakeLists.txt") and
// is surfaced in init's summary; `description` is the one-line agent description
// passed to create_agent.
//
// ☠️ `marker` never reaches an artifact: the agent YAML carries `description`
// (fixed per specialty) and the vault CONTEXT.md comes from
// specialty_context_md(name, language). That is what keeps the C++ path and the
// setup-knowers.sh path byte-identical even though they report different paths.
struct RepoSpecialty {
    std::string name;         // e.g. "move-dev"
    std::string language;     // e.g. "Sui Move 2024"
    std::string marker;       // marker path relative to the project root
    std::string description;  // one-line agent description
};

namespace detect_detail {

// Child directories never scanned for a marker. A marker inside any of these
// describes a dependency, a build artifact or a fixture -- not the project.
// Mirrored, case for case, by find_marker() in scripts/setup-knowers.sh.
inline bool is_skipped_child_dir(const std::string& name) {
    if (name.empty() || name[0] == '.') return true;  // .git, .quorum, dot-dirs
    if (name.rfind("build", 0) == 0) return true;     // build, build-w1, builds
    static const char* const kSkip[] = {
        "node_modules", "dist", "target", "templates", "sample",
    };
    for (const auto* s : kSkip) {
        if (name == s) return true;
    }
    return false;
}

inline bool is_marker_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);  // matches the shell mirror's `[ -f ]`
}

}  // namespace detect_detail

// Detect language specialties from marker files at the ROOT of `dir` or ONE
// level down. Per language the first hit wins: the root is checked first, then
// the immediate child directories in sorted order. Depth 2 and below is never
// scanned, and the skip list above is honoured. A repo may match several
// languages; that order stays deterministic: move-dev, ts-dev, cpp-dev.
//
// Motivating case: a repo whose root carries no marker because each language
// lives in its own subdirectory (Quorum itself -- quorum-core/CMakeLists.txt
// and quorum-web/package.json).
inline std::vector<RepoSpecialty> detect_repo_specialties(const fs::path& dir) {
    const std::vector<RepoSpecialty> languages = {
        {"move-dev", "Sui Move 2024", "Move.toml",
         "move-dev: Sui Move 2024 smart-contract doer — implements and tests "
         "Move modules; invokes the sui-dev-skills + move-code-quality skills"},
        {"ts-dev", "TypeScript", "package.json",
         "ts-dev: TypeScript doer — implements and tests TypeScript; matches "
         "the project's existing lint/format config (no extra skill)"},
        {"cpp-dev", "C++", "CMakeLists.txt",
         "cpp-dev: C++ doer — implements and tests C++ code; invokes the "
         "cpp-code-quality skill"},
    };

    // Scannable immediate children, sorted by name so "first hit wins" is
    // reproducible regardless of directory-iteration order.
    std::vector<std::string> children;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        std::error_code dir_ec;
        if (!entry.is_directory(dir_ec)) continue;
        auto name = entry.path().filename().string();
        if (detect_detail::is_skipped_child_dir(name)) continue;
        children.push_back(name);
    }
    std::sort(children.begin(), children.end());

    std::vector<RepoSpecialty> out;
    for (const auto& lang : languages) {
        std::string found;  // marker path relative to `dir`
        if (detect_detail::is_marker_file(dir / lang.marker)) {
            found = lang.marker;  // root wins
        } else {
            for (const auto& child : children) {
                if (detect_detail::is_marker_file(dir / child / lang.marker)) {
                    found = child + "/" + lang.marker;
                    break;
                }
            }
        }
        if (found.empty()) continue;
        RepoSpecialty sp = lang;
        sp.marker = found;
        out.push_back(sp);
    }
    return out;
}

// Build the deterministic, project-agnostic CONTEXT.md for a specialty doer.
//
// ☠️ This content is mirrored, byte-for-byte, by write_specialty_context() in
// scripts/setup-knowers.sh (which seeds specialties into ALREADY-initialized
// projects). The two MUST move together — same precedent as the knower
// descriptions duplicated between this file and that script.
inline std::string specialty_context_md(const std::string& name,
                                        const std::string& language) {
    std::string domain;  // "## Domain skills" body (ends with a newline)
    std::string gate;    // "## Gate discipline" body (ends with a newline)
    if (name == "move-dev") {
        domain =
            "Behavioral patterns come from the **quorum-roles/doer** skill (loaded via `skill_file`). Invoke these globally-installed domain skills when you write Move:\n"
            "\n"
            "- **sui-dev-skills** — Move contracts, the Sui object model, PTBs, on-chain testing\n"
            "- **move-code-quality** — the Move 2024 code-quality checklist\n";
        gate =
            "- Run `sui move build` AND `sui move test` before declaring a task done.\n"
            "- Report the ACTUAL result line (paste the `Test result:` line). A gate you didn't execute is not a gate.\n";
    } else if (name == "cpp-dev") {
        domain =
            "Behavioral patterns come from the **quorum-roles/doer** skill (loaded via `skill_file`). Invoke this globally-installed domain skill when you write C++:\n"
            "\n"
            "- **cpp-code-quality** — the C++ code-quality checklist\n";
        gate =
            "- Run the project's cmake build AND `ctest` before declaring a task done.\n"
            "- Report the ACTUAL result line (paste the ctest pass/fail summary). A gate you didn't execute is not a gate.\n";
    } else {  // ts-dev (and any other TS-flavored specialty)
        domain =
            "Behavioral patterns come from the **quorum-roles/doer** skill (loaded via `skill_file`). There is no TypeScript-specific skill — write plain TypeScript and match the project's existing lint/format config (eslint/prettier/tsconfig).\n";
        gate =
            "- Run the project's `build` / `typecheck` scripts (and its test script, if one exists) before declaring a task done.\n"
            "- Report the ACTUAL result line. A gate you didn't execute is not a gate.\n";
    }

    std::string s;
    s += "# " + name + " — Agent Context\n";
    s += "\n";
    s += "## Role\n";
    s += "\n";
    s += "You are the **" + name + "** (doer). A " + language +
         " developer for this project. You implement and test; you do not route or plan.\n";
    s += "\n";
    s += "## Domain skills\n";
    s += "\n";
    s += domain;
    s += "\n";
    s += "## Repo conventions\n";
    s += "\n";
    s += "(fill in per project — replace these placeholders with the specifics)\n";
    s += "\n";
    s += "- Verified build/test commands — the exact invocations that pass on this machine.\n";
    s += "- Edition / toolchain pins — compiler, framework, and language-edition versions.\n";
    s += "- The ground-truth design doc — the file that outranks tickets and code.\n";
    s += "- Mock seams — what is mocked, and where the real-implementation markers live.\n";
    s += "\n";
    s += "## Gate discipline (non-negotiable)\n";
    s += "\n";
    s += gate;
    // universal_rules_for_role() begins with "\n## Universal Rules\n\n..." — the
    // leading newline yields the blank line between the gate block and the header.
    s += universal_rules_for_role("doer");
    return s;
}

// Append-only .gitignore maintenance (F7). Reads existing lines (file may be
// absent), compares after trimming trailing whitespace/CR, and appends only the
// entries not already present — under a single "# added by quorum init" comment
// (written once, and only when appending ≥1 entry and the marker isn't already
// present). Existing content is preserved byte-for-byte as a prefix. Returns the
// number of entries appended. Best-effort: never throws init into failure.
inline int ensure_gitignore_entries(const fs::path& gitignore_path,
                                     const std::vector<std::string>& entries) {
    std::string raw;
    bool had_content = false;
    bool ends_with_newline = true;  // absent/empty file = clean boundary
    std::vector<std::string> existing;
    if (fs::exists(gitignore_path)) {
        std::ifstream in(gitignore_path, std::ios::binary);
        raw.assign(std::istreambuf_iterator<char>(in),
                   std::istreambuf_iterator<char>());
        had_content = !raw.empty();
        ends_with_newline = raw.empty() || raw.back() == '\n';
        std::istringstream ss(raw);
        std::string line;
        while (std::getline(ss, line)) {
            size_t end = line.find_last_not_of(" \t\r");
            existing.push_back(end == std::string::npos ? std::string()
                                                        : line.substr(0, end + 1));
        }
    }

    auto contains = [&existing](const std::string& e) {
        for (const auto& l : existing) if (l == e) return true;
        return false;
    };

    std::vector<std::string> to_add;
    for (const auto& e : entries) {
        bool already = contains(e);
        for (const auto& a : to_add) if (a == e) already = true;  // dedup in-call
        if (!already) to_add.push_back(e);
    }
    if (to_add.empty()) return 0;

    const std::string marker = "# added by quorum init";
    const bool has_marker = contains(marker);

    std::ofstream out(gitignore_path, std::ios::binary | std::ios::app);
    if (!out.is_open()) return 0;  // best-effort; init must not fail on this

    if (had_content && !ends_with_newline) out << "\n";  // close a dangling line
    if (!has_marker) {
        if (had_content) out << "\n";  // blank separator before our block
        out << marker << "\n";
    }
    for (const auto& e : to_add) out << e << "\n";
    return static_cast<int>(to_add.size());
}

// Resolve the shipped SKILL.md for a knower specialty. First hit wins:
//   (a) <quorum_root>/templates/skills/<knower>/SKILL.md  (when known)
//   (b) $HOME/.claude/skills/<knower>/SKILL.md            (installed skills)
//   (c) CWD ladder: templates/.. then ../templates/..     (mirrors rubric.h)
// Returns an empty string if none resolve (init must NOT fail on this).
inline std::string resolve_knower_skill(const std::string& quorum_root,
                                        const std::string& knower) {
    if (!quorum_root.empty()) {
        auto p = fs::path(quorum_root) / "templates" / "skills" / knower / "SKILL.md";
        if (fs::exists(p) && fs::is_regular_file(p)) return p.string();
    }
    if (const char* home = std::getenv("HOME")) {
        auto p = fs::path(home) / ".claude" / "skills" / knower / "SKILL.md";
        if (fs::exists(p) && fs::is_regular_file(p)) return p.string();
    }
    const std::vector<fs::path> candidates = {
        fs::path("templates") / "skills" / knower / "SKILL.md",
        fs::path("..") / "templates" / "skills" / knower / "SKILL.md",
    };
    for (const auto& c : candidates) {
        if (fs::exists(c) && fs::is_regular_file(c)) return c.string();
    }
    return "";
}

inline int init_project(const std::string& quorum_root = "") {
    // 1. Check .quorum/ doesn't already exist
    if (fs::exists(".quorum")) {
        std::cerr << "ERROR: already initialized (.quorum/ exists)\n";
        return 1;
    }

    auto cwd = fs::current_path().string();
    std::cout << "Initializing Quorum in " << cwd << "\n";

    // 2. Create directories
    fs::create_directories(".quorum/agents");
    fs::create_directories(".quorum/vaults/leader/knowledge");
    // Project-scope knowledge: rules/refs that apply to ALL agents.
    // Phase 7 Track 2 — context_assembler resolves rules across project,
    // role, and per-agent vault scopes, with the cap operating on the union.
    fs::create_directories(".quorum/knowledge");
    {
        std::ofstream gk(".quorum/knowledge/.gitkeep", std::ios::trunc);
        // Empty file — keeps the directory under version control.
    }

    // Role-scope knowledge (Phase 7 Track 3): rules under
    // .quorum/knowledge/roles/<role>/ apply to every agent of that role.
    // Pre-create one subdir per built-in role so the convention is
    // discoverable from a fresh init.
    static constexpr const char* kRoles[] = {
        "leader", "thinker", "doer", "evaluator",
    };
    for (const auto* role : kRoles) {
        auto dir = std::string(".quorum/knowledge/roles/") + role;
        fs::create_directories(dir);
        std::ofstream gk(dir + "/.gitkeep", std::ios::trunc);
    }

    // 3. Write .quorum/config.yaml
    {
        std::ofstream out(".quorum/config.yaml", std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "ERROR: cannot write .quorum/config.yaml\n";
            return 1;
        }
        out << "# Quorum Project Configuration\n"
            << "# Generated by quorum init\n"
            << "\n"
            << "daemon:\n"
            << "  data_dir: .quorum\n"
            << "  pid_file: .quorum/quorum.pid\n"
            << "\n"
            << "budget:\n"
            << "  window_budget_usd: 100.00\n"
            << "  window_hours: 5\n"
            << "\n"
            << "conversations:\n"
            << "  enabled: true\n"
            << "  default_max_rounds: 20\n"
            << "  leader: leader\n";
    }
    std::cout << "  Created: .quorum/config.yaml\n";

    // 4. Write .quorum/agents/leader.yaml
    {
        std::ofstream out(".quorum/agents/leader.yaml", std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "ERROR: cannot write .quorum/agents/leader.yaml\n";
            return 1;
        }
        out << "id: leader\n"
            << "name: \"Leader\"\n"
            << "role: leader\n"
            << "description: \"Coordinates the team -- receives goals, routes work to thinkers/knowers/doers\"\n"
            << "\n"
            << "vault_path: .quorum/vaults/leader/\n"
            << "context_file: .quorum/vaults/leader/CONTEXT.md\n";
    }
    std::cout << "  Created: .quorum/agents/leader.yaml\n";

    // 5. Write .quorum/vaults/leader/CONTEXT.md
    {
        std::ofstream out(".quorum/vaults/leader/CONTEXT.md", std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "ERROR: cannot write .quorum/vaults/leader/CONTEXT.md\n";
            return 1;
        }
        out << "# Leader Agent\n"
            << "\n"
            << "## Role\n"
            << "\n"
            << "You are the **Leader**. You receive the user's goal, break it into steps,\n"
            << "and route work to the right agent via HANDOFF blocks.\n"
            << "\n"
            << "## What You Do\n"
            << "\n"
            << "1. Receive the goal from the user.\n"
            << "2. Decide which agent should handle the next step.\n"
            << "3. Route via a HANDOFF block with a clear, actionable prompt.\n"
            << "4. When the goal is complete, HANDOFF to `done` to finish the conversation.\n"
            << "\n"
            << "## What You Do NOT Do\n"
            << "\n"
            << "- Do NOT write code yourself.\n"
            << "- Do NOT plan in excessive detail -- delegate to thinker agents.\n"
            << "- Do NOT accumulate knowledge yourself -- the knowers (cartographer/architect/historian/recap) are the sole accumulators.\n"
            << "\n"
            << "## When Stuck\n"
            << "\n"
            << "If you need clarification or cannot proceed, HANDOFF to `human` with a\n"
            << "clear question explaining what you need.\n";
    }
    std::cout << "  Created: .quorum/vaults/leader/CONTEXT.md\n";

    // 6. Write .quorum/.gitignore
    {
        std::ofstream out(".quorum/.gitignore", std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "ERROR: cannot write .quorum/.gitignore\n";
            return 1;
        }
        out << "# Quorum runtime data -- regenerated by daemon\n"
            << "quorum.db\n"
            << "quorum.db-wal\n"
            << "quorum.db-shm\n"
            << "quorum.pid\n"
            << "vaults/*/knowledge/\n"
            << "\n"
            << "# Keep: config.yaml, agents/\n";
    }
    std::cout << "  Created: .quorum/.gitignore\n";

    // 6a. Create quorum.db with schema
    {
        sui::quorum::Database db(".quorum/quorum.db");
        if (db.is_open()) {
            sui::quorum::create_schema(db);
            std::cout << "  Created: .quorum/quorum.db\n";
        }
    }

    // 6b. Scaffold the rest of the default roster (definitions only — no
    // Tier-1 scans, no Python tool copies, no recap dump seeding; those stay
    // in scripts/setup-knowers.sh). Every agent is created with no_ai = true
    // so init spends ZERO tokens. CWD is the fresh project, so create_agent
    // discovers THIS .quorum/ via discover_project_root().
    //
    // Generic role: thinker relies on skill auto-detect (OK if it falls back
    // to a generic CONTEXT.md template).
    {
        sui::quorum::cli::AgentCreateParams p;
        p.role = "thinker";
        p.name = "thinker";
        p.no_ai = true;
        sui::quorum::cli::create_agent(p);
    }

    // Knowers: role `thinker` + a specialty SKILL file. Descriptions are
    // byte-identical to scripts/setup-knowers.sh (the source of truth). If a
    // SKILL doesn't resolve, the agent is still created WITHOUT a skill_file
    // and a warning is printed — init must NOT fail on a missing SKILL.
    struct Knower { const char* name; const char* description; };
    static const Knower kKnowers[] = {
        {"cartographer",
         "Cartographer: knows the project layout. Reads the Tier-1 index (.quorum/cartographer/layout.json) + honors the root CLAUDE.md; produces a fast-lookup project index. Read-only."},
        {"architect",
         "Architect: maps component interconnections (imports, cross-repo calls, event flows) with file evidence; traces the primary flow; flags coupling/invariants. Read-only."},
        {"historian",
         "Historian: knows the project's decisions + pivots. Reads the Tier-1 record (.quorum/historian/decisions-raw.json) + the Decision Log; tracks status/supersession with PR/commit provenance. Read-only."},
        {"recap",
         "Recap: knows what changed recently + where you left off (WHAT/WHEN). Reads the Tier-1 windowed timeline (.quorum/recap/timeline-raw.json) + operator-dumped timestamped messages, weaves one dated component-grouped timeline, drafts where-i-left-off, with a by-intent read-only Linear status overlay. Read-only; never queries Linear/Slack/Telegram."},
    };
    for (const auto& k : kKnowers) {
        sui::quorum::cli::AgentCreateParams p;
        p.role = "thinker";
        p.name = k.name;
        p.description = k.description;
        p.no_ai = true;
        p.skill_file = resolve_knower_skill(quorum_root, k.name);
        if (p.skill_file.empty()) {
            std::cout << "  WARNING: " << k.name << " SKILL not found; run "
                      << "scripts/setup-knowers.sh to attach the specialty skill.\n";
        }
        sui::quorum::cli::create_agent(p);
    }

    // 6c. Auto-attach language specialty doers (F10). For each root-level
    // marker file detected in the CWD, create a no_ai doer (zero tokens) via
    // the same create_agent path, then OVERWRITE its CONTEXT.md with the
    // deterministic, project-agnostic specialty context (like the leader
    // CONTEXT.md at step 5). create_agent auto-detects the quorum-roles/doer
    // SKILL from $HOME (matches the Crucible move-dev reference shape).
    auto specialties = detect_repo_specialties(fs::current_path());
    std::vector<RepoSpecialty> created_specialties;
    for (const auto& sp : specialties) {
        sui::quorum::cli::AgentCreateParams p;
        p.role = "doer";
        p.name = sp.name;
        p.description = sp.description;
        p.target_dir = ".";
        p.no_ai = true;
        if (sui::quorum::cli::create_agent(p) == 0) {
            auto ctx = std::string(".quorum/vaults/") + sp.name + "/CONTEXT.md";
            std::ofstream out(ctx, std::ios::trunc);
            if (out.is_open()) out << specialty_context_md(sp.name, sp.language);
            created_specialties.push_back(sp);
            std::cout << "  Auto-attached specialty doer: " << sp.name
                      << " (detected " << sp.marker << ")\n";
        }
    }

    // 6d. Seed/extend the project's ROOT .gitignore (F7): always ignore IDE +
    // OS cruft; add per-specialty build output. Append-only — never rewrites
    // existing content (that's why the dogfood committed .idea/).
    {
        std::vector<std::string> ignore = {".DS_Store", ".idea/", ".vscode/"};
        for (const auto& sp : created_specialties) {
            if (sp.name == "move-dev") {
                ignore.push_back("build/");
            } else if (sp.name == "cpp-dev") {
                ignore.push_back("build/");
                ignore.push_back("cmake-build-*/");
            } else if (sp.name == "ts-dev") {
                ignore.push_back("node_modules/");
                ignore.push_back("dist/");
            }
        }
        int added = ensure_gitignore_entries(".gitignore", ignore);
        if (added > 0) {
            std::cout << "  .gitignore: appended " << added << " entr"
                      << (added == 1 ? "y" : "ies")
                      << " (IDE/OS cruft + build output)\n";
        } else {
            std::cout << "  .gitignore: already covers the recommended entries\n";
        }
    }

    // 7. Print next steps
    std::cout << "  Created: .quorum/vaults/leader/knowledge/\n";
    std::cout << "  Created: .quorum/knowledge/  "
              << "(project-wide rules and references that apply to all agents)\n";
    std::cout << "  Created: .quorum/knowledge/roles/{leader,thinker,doer,evaluator}/  "
              << "(role-specific rules apply to every agent of that role)\n";
    const int total_agents = 6 + static_cast<int>(created_specialties.size());
    std::cout << "\nQuorum initialized with " << total_agents << " agents "
              << "(leader, thinker, cartographer, architect, historian, recap";
    for (const auto& sp : created_specialties) std::cout << ", " << sp.name;
    std::cout << ").\n";
    if (!created_specialties.empty()) {
        std::cout << "Auto-attached language specialties (why):\n";
        for (const auto& sp : created_specialties) {
            std::cout << "  - " << sp.name << " — " << sp.language
                      << ", from " << sp.marker << "\n";
        }
    }
    std::cout << "Next steps:\n";
    std::cout << "  1. Attach knower specialty skills + Tier-1 scans (token-free):\n";
    std::cout << "     scripts/setup-knowers.sh <project-dir>\n";
    std::cout << "  2. Add more agents (auto-discovered from .quorum/agents/):\n";
    std::cout << "     quorum agent create --role doer --name my-dev\n";
    std::cout << "  3. Start a conversation:\n";
    std::cout << "     quorum converse \"your goal here\"\n";

    return 0;
}

} // namespace sui::quorum::cli
