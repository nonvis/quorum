#pragma once

// Phase 13 Track 3 — `quorum supervisor init`.
//
// A DETERMINISTIC config generator (NO `claude -p`) that GENERATES the autopilot
// flight-plan config `SUPERVISOR.md` at the project root and scaffolds the
// `.quorum/autopilot/checkpoint.md` resume state. `SUPERVISOR.md` is generated,
// never hand-authored (Decision #23 principle); like `quorum init` /
// `quorum vault dedup`, it is foolproof and idempotent (never clobbers an
// existing SUPERVISOR.md without --force, and never clobbers a resume-state
// checkpoint at all).
//
// The two artifacts and their schemas are defined in
// templates/specs/autopilot-protocol.md (v0.1) — the sections
//   "## `SUPERVISOR.md` — file location + schema"  and
//   "## `.quorum/autopilot/checkpoint.md` — schema"
// are authoritative; the generators below match them so the supervisor SKILL's
// startup gate can read them.
//
// The roster table is auto-filled from <root>/.quorum/agents/*.yaml using the
// same flat-YAML parse the daemon's `quorum ask` uses (list_agent_names +
// parse_agent_field, mirrored from cli/ask.h to keep this header self-contained).
//
// generate_supervisor_md / render_checkpoint_skeleton are PURE (filesystem reads
// only, no claude tokens), so they are unit-tested directly
// (tests/unit/test_supervisor_init.cpp).
//
// Header-only, matches the cli/ask.h convention.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "utils/file_io.h"   // detail::atomic_write_text, detail::read_file_text

namespace sui::quorum::cli {

struct SupervisorInitOptions {
    std::string project_path;   // resolved target project root (sibling of .quorum/)
    bool force = false;         // --force: regenerate SUPERVISOR.md even if present
};

namespace detail {

// ---- flat-YAML agent parsing (mirrored from cli/ask.h, kept self-contained) --

// List the available agent name stems (filename without ".yaml") under
// <project_root>/.quorum/agents/, sorted ascending. PURE: filesystem only.
[[nodiscard]] inline std::vector<std::string> sup_list_agent_names(
    const std::string& project_root) {
    namespace fs = std::filesystem;
    std::vector<std::string> names;
    auto dir = fs::path(project_root) / ".quorum" / "agents";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return names;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".yaml") continue;
        names.push_back(e.path().stem().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

// Strip a single pair of surrounding double-quotes (and trim outer whitespace)
// from a flat-YAML scalar value. PURE.
[[nodiscard]] inline std::string sup_unquote_yaml_value(std::string v) {
    auto l = v.find_first_not_of(" \t");
    auto r = v.find_last_not_of(" \t\r\n");
    if (l == std::string::npos) return {};
    v = v.substr(l, r - l + 1);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        v = v.substr(1, v.size() - 2);
    }
    return v;
}

// Parse a single top-level `key: value` field from a flat-YAML agent config.
// Returns the unquoted value, or "" if the key is absent. Exact-key match so
// "skill_file" does not shadow a hypothetical "skill". PURE.
[[nodiscard]] inline std::string sup_parse_agent_field(
    const std::string& yaml_text, const std::string& key) {
    std::istringstream in(yaml_text);
    std::string line;
    while (std::getline(in, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon);
        auto kl = k.find_first_not_of(" \t");
        auto kr = k.find_last_not_of(" \t");
        if (kl == std::string::npos) continue;
        k = k.substr(kl, kr - kl + 1);
        if (k != key) continue;
        return sup_unquote_yaml_value(line.substr(colon + 1));
    }
    return {};
}

// Current UTC time as full ISO-8601 "YYYY-MM-DDTHH:MM:SSZ". Uses gmtime_r and
// manual formatting (no strftime dependency), keeping the header self-contained.
[[nodiscard]] inline std::string utc_now_iso() {
    std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
    ::gmtime_r(&now, &tm_buf);
    char out[32];
    std::snprintf(out, sizeof(out), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return std::string(out);
}

}  // namespace detail

// Generate the `SUPERVISOR.md` flight-plan config for the project at
// `project_root`. Matches the canonical structure in autopilot-protocol.md
// "## `SUPERVISOR.md` — file location + schema". The Roster table is auto-filled
// from <root>/.quorum/agents/*.yaml; if there are no agents, the table body is
// replaced by the "(no agents configured ...)" line. The Flight plan is a single
// placeholder task so the supervisor's startup gate stops until the operator
// fills it. PURE: filesystem reads only, no claude tokens.
[[nodiscard]] inline std::string generate_supervisor_md(
    const std::string& project_root) {
    namespace fs = std::filesystem;
    fs::path root(project_root);

    // Project name = basename of the root (handle a trailing slash).
    std::string project_name = root.filename().string();
    if (project_name.empty()) {
        project_name = root.parent_path().filename().string();
    }

    std::string out;

    // Frontmatter.
    out += "---\n";
    out += "title: Autopilot flight plan\n";
    out += "generated_by: quorum supervisor init\n";
    out += "spec_version: 0.3\n";
    out += "project_root: " + project_root + "\n";
    out += "---\n\n";

    out += "# SUPERVISOR.md — Autopilot Flight Plan\n\n";

    // Project.
    out += "## Project\n\n";
    out += "- name: " + project_name + "\n";
    out += "- root: " + project_root + "\n\n";

    // Roster — auto-filled from .quorum/agents/*.yaml.
    out += "## Roster (subagent workers)\n\n";
    out += "| agent | role | skill |\n";
    out += "|-------|------|-------|\n";
    {
        auto names = detail::sup_list_agent_names(project_root);
        if (names.empty()) {
            out += "(no agents configured — run `quorum agent create` first)\n";
        } else {
            for (const auto& name : names) {
                auto yaml_path = root / ".quorum" / "agents" / (name + ".yaml");
                auto yaml_text = sui::quorum::detail::read_file_text(yaml_path);
                auto role = detail::sup_parse_agent_field(yaml_text, "role");
                auto skill = detail::sup_parse_agent_field(yaml_text, "skill_file");
                const std::string em_dash = "\xe2\x80\x94";  // "—"
                out += "| " + name + " | " +
                       (role.empty() ? em_dash : role) + " | " +
                       (skill.empty() ? em_dash : skill) + " |\n";
            }
        }
    }
    out += "\n";

    // Record-keeping (Phase 14) — knowers are the sole accumulators. At
    // end-of-flight, refresh the affected knowers so their vault surveys
    // re-survey the changed codebase (the daemon's generic path recommends the
    // same; autopilot auto-runs it).
    out += "## Record-keeping (knower refresh — end of flight)\n\n";
    out += "- The knowers are the sole accumulators. There is no scribe and no "
           "learnings.md.\n";
    out += "- At end-of-flight, refresh the affected knowers so their surveys "
           "re-survey the changed code:\n";
    out += "  `quorum knower refresh --project " + project_root + " --all`\n";
    out += "  (or a single lens: `--knower <cartographer|architect|historian|"
           "recap>`)\n\n";
    out += "Humans read project state on demand via `quorum ask` (knower "
           "surveys + live code) or\n";
    out += "`quorum ask --agent recap`. There is no separate curated layer to "
           "maintain.\n\n";

    // Stop conditions — the four bullets per spec.
    out += "## Stop conditions\n\n";
    out += "- context_near_full: checkpoint + write morning review + STOP\n";
    out += "- window_exhausted: STOP at the window edge\n";
    out += "- needs_human: STOP, leave the question in the morning review\n";
    out += "- max_major_tasks: \xe2\x80\x94\n\n";

    // Flight plan — one placeholder task (operator fills/extends). This is
    // intentionally a placeholder so the startup gate stops until edited.
    out += "## Flight plan\n\n";
    out += "### Task 1: <replace with your first major task>\n";
    out += "- agent: <pick a roster agent>\n";
    out += "- slices (parallel):\n";
    out += "  - <first slice prompt>\n";
    out += "- done when: <criteria>\n";

    return out;
}

// Render the empty `.quorum/autopilot/checkpoint.md` skeleton. Matches the
// canonical structure in autopilot-protocol.md "## `.quorum/autopilot/
// checkpoint.md` — schema": the supervisor populates the Major tasks list from
// SUPERVISOR.md on first run, so this scaffold is intentionally empty. `utc` is
// written to both `Created at:` and `Updated at:`. PURE.
[[nodiscard]] inline std::string render_checkpoint_skeleton(
    const std::string& utc) {
    std::string out;
    out += "# Autopilot checkpoint\n\n";
    out += "Created at: " + utc + "\n";
    out += "Updated at: " + utc + "\n";
    out += "Flight spec: 0.2\n\n";
    out += "## Major tasks\n\n";
    out += "(populated from SUPERVISOR.md on first run)\n\n";
    out += "## Condensed outcomes\n\n";
    out += "(populated after each major task)\n\n";
    out += "## Morning review\n\n";
    out += "- done: none yet\n";
    out += "- pending: (populated on first run)\n";
    out += "- blocked-on: none\n";
    return out;
}

// Top-level entrypoint for `quorum supervisor init`. DETERMINISTIC (no claude):
//   1. Resolve project_root (default cwd). Require <root>/.quorum to exist.
//   2. Generate SUPERVISOR.md at <root>/SUPERVISOR.md unless it exists and
//      !force (foolproof: never clobber operator edits without --force).
//   3. Scaffold <root>/.quorum/autopilot/ + checkpoint.md ONLY if checkpoint.md
//      is absent (never clobber a resume state).
//   4. Print what was generated + the next-step hint.
// Returns 0 on success (including the "already exists" no-op), 1 on hard error.
[[nodiscard]] inline int run_supervisor_init(const SupervisorInitOptions& opts) {
    namespace fs = std::filesystem;

    // 1. Resolve project root (default cwd) + require .quorum/.
    std::string project_root = opts.project_path;
    if (project_root.empty()) {
        project_root = fs::current_path().string();
    }
    fs::path root(project_root);

    std::error_code ec;
    if (!fs::exists(root / ".quorum", ec)) {
        std::cerr << "ERROR: no .quorum/ found at " << project_root
                  << " — run 'quorum init' first\n";
        return 1;
    }

    // 2. SUPERVISOR.md — generate unless present and !force.
    auto supervisor_path = root / "SUPERVISOR.md";
    bool supervisor_exists = fs::exists(supervisor_path, ec);
    if (supervisor_exists && !opts.force) {
        std::cout << "SUPERVISOR.md already exists at "
                  << supervisor_path.string()
                  << " (use --force to regenerate). Not overwriting.\n";
    } else {
        auto content = generate_supervisor_md(project_root);
        std::string err;
        if (!sui::quorum::detail::atomic_write_text(supervisor_path, content,
                                                    err)) {
            std::cerr << "ERROR: " << err << "\n";
            return 1;
        }
        std::cout << (supervisor_exists ? "Regenerated " : "Generated ")
                  << supervisor_path.string() << "\n";
    }

    // 3. Scaffold the autopilot checkpoint — never clobber a resume state.
    auto autopilot_dir = root / ".quorum" / "autopilot";
    auto checkpoint_path = autopilot_dir / "checkpoint.md";
    if (fs::exists(checkpoint_path, ec)) {
        std::cout << "Checkpoint already exists at " << checkpoint_path.string()
                  << " (preserving resume state).\n";
    } else {
        fs::create_directories(autopilot_dir, ec);
        if (ec) {
            std::cerr << "ERROR: failed to create " << autopilot_dir.string()
                      << ": " << ec.message() << "\n";
            return 1;
        }
        auto skeleton = render_checkpoint_skeleton(detail::utc_now_iso());
        std::string err;
        if (!sui::quorum::detail::atomic_write_text(checkpoint_path, skeleton,
                                                    err)) {
            std::cerr << "ERROR: " << err << "\n";
            return 1;
        }
        std::cout << "Scaffolded " << checkpoint_path.string() << "\n";
    }

    // 4. Next-step hint.
    std::cout << "next: edit the Flight plan in SUPERVISOR.md, then run "
                 "`claude --agent supervisor`\n";
    return 0;
}

}  // namespace sui::quorum::cli
