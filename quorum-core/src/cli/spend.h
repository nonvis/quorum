#pragma once

// `quorum spend [--project <path|name>] [--since <ISO8601>] [--until <ISO8601>]
//               [--json]`.
//
// Per-run token/$ spend readout from the Claude Code transcripts (deterministic,
// no-LLM, $0). The autopilot supervisor runs this at halt to report token spend
// against the window budget; it copies the total + budget-comparison line into
// the morning review BEFORE removing `.quorum/autopilot/LOCK` (spend reads the
// LOCK's line-1 flight-start time as its --since fallback, so it must run first).
//
// IMPLEMENTATION CHOICE — SHELL OUT to scripts/spend_readout.py (not reimplement
// the transcript walk in C++):
//   - The transcript JSONL parse + per-model token sum + $ estimate + SQLite
//     cross-check is naturally a stdlib-Python job (json/sqlite3), and keeps the
//     numeric logic in ONE testable place. This mirrors knower_refresh.h shelling
//     out to run-knower.sh rather than duplicating its map.
//   - The script is reachable robustly: the `quorum` CLI binary lives at
//     <repo>/build/quorum_daemon and `make install` symlinks ~/.local/bin/quorum
//     -> that binary, so quorum_repo_root_from_exe(argv[0]) (set in main.cpp,
//     same as run_knower_refresh) resolves back into <repo>;
//     <repo>/scripts/spend_readout.py is then always findable.
//
// The PURE helpers (transcript_dir_for, lock_started_at) live in spend_detail
// below and are unit-tested directly (tests/unit/test_spend.cpp). The only
// process I/O is the python3 shell-out inside run_spend — and it spends $0
// (read-only transcript scan, no claude).
//
// Header-only, matches the cli/knower_refresh.h convention.

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <sys/wait.h>   // WIFEXITED / WEXITSTATUS

#include "cli/ask.h"    // resolve_project_path (reused, not reimplemented)

namespace sui::quorum::cli {

struct SpendOptions {
    std::string project;        // path OR project name; empty = cwd (".")
    std::string since;          // --since <ISO8601>; empty -> LOCK line-1 fallback
    std::string until;          // --until <ISO8601>; empty -> the script uses now
    bool json = false;          // --json: emit one JSON object
    // Resolved during run: the repo root that contains scripts/spend_readout.py.
    // Set from argv[0] in main.cpp (quorum_repo_root_from_exe); empty = let the
    // relative-path fallback find it.
    std::string quorum_root;
};

namespace spend_detail {

// Munge an ABSOLUTE project root to its Claude Code transcript dir:
//   <HOME>/.claude/projects/<abs project with every '/' and '.' -> '-'>
// e.g. /Users/sangsoo/nonvis/crucible ->
//      <HOME>/.claude/projects/-Users-sangsoo-nonvis-crucible
// HOME from getenv; empty-HOME guard returns "" (a caller with no HOME can't
// have transcripts). PURE.
[[nodiscard]] inline std::string transcript_dir_for(
    const std::string& abs_project_root) {
    const char* h = std::getenv("HOME");
    std::string home = h ? std::string(h) : std::string{};
    if (home.empty()) return {};
    std::string munged;
    munged.reserve(abs_project_root.size());
    for (char c : abs_project_root)
        munged += (c == '/' || c == '.') ? '-' : c;
    return home + "/.claude/projects/" + munged;
}

// Read line 1 of <root>/.quorum/autopilot/LOCK if it exists and line 1 parses as
// an ISO-timestamp SHAPE (cheap check: 4 digits then '-', e.g. "2026-..."), else
// "". This is the --since fallback: the LOCK's first line is the flight start
// time (written per supervisor SKILL Step 0.4). PURE (filesystem read only).
[[nodiscard]] inline std::string lock_started_at(
    const std::string& project_root) {
    namespace fs = std::filesystem;
    fs::path lock = fs::path(project_root) / ".quorum" / "autopilot" / "LOCK";
    std::error_code ec;
    if (!fs::exists(lock, ec)) return {};
    std::ifstream in(lock);
    if (!in) return {};
    std::string line;
    if (!std::getline(in, line)) return {};
    // Trim surrounding whitespace / CR.
    auto l = line.find_first_not_of(" \t\r\n");
    auto r = line.find_last_not_of(" \t\r\n");
    if (l == std::string::npos) return {};
    line = line.substr(l, r - l + 1);
    // Cheap ISO shape gate: "YYYY-" (4 digits + '-').
    if (line.size() < 5) return {};
    for (int i = 0; i < 4; ++i)
        if (!std::isdigit(static_cast<unsigned char>(line[i]))) return {};
    if (line[4] != '-') return {};
    return line;
}

// Resolve <quorum_root>/scripts/spend_readout.py. Empty quorum_root -> the
// relative "scripts/spend_readout.py" last-resort fallback. PURE.
[[nodiscard]] inline std::string spend_script(const std::string& quorum_root) {
    namespace fs = std::filesystem;
    if (quorum_root.empty()) return "scripts/spend_readout.py";
    return (fs::path(quorum_root) / "scripts" / "spend_readout.py").string();
}

}  // namespace spend_detail

// Top-level entrypoint for `quorum spend`. Resolves the project (reusing
// cli/ask.h's resolve_project_path); derives --since from opts.since, else the
// LOCK's line-1 flight-start time, else errors; then std::system's the
// python3 spend_readout.py invocation (live output — it's fast, read-only, $0)
// and propagates its exit code.
[[nodiscard]] inline int run_spend(const SpendOptions& opts) {
    namespace fs = std::filesystem;
    using namespace spend_detail;

    // 1. Resolve project root (default cwd) via the SAME helper `quorum ask` /
    //    knower refresh use.
    std::string arg = opts.project.empty() ? std::string(".") : opts.project;
    std::string err;
    std::string project_root = resolve_project_path(arg, err);
    if (project_root.empty()) {
        std::cerr << "ERROR: " << err << "\n";
        return 1;
    }

    // 2. since: --since, else the flight start on the LOCK's line 1, else error.
    std::string since = opts.since;
    if (since.empty()) since = lock_started_at(project_root);
    if (since.empty()) {
        std::cerr << "ERROR: no --since and no .quorum/autopilot/LOCK — pass "
                     "--since <ISO8601>\n";
        return 1;
    }

    // 3. Locate scripts/spend_readout.py via quorum_root (same pattern as
    //    run_knower_script). Bail with a clear message if the repo is not intact.
    auto script = spend_script(opts.quorum_root);
    {
        std::error_code ec;
        if (!fs::exists(script, ec)) {
            std::cerr << "ERROR: spend_readout.py not found at " << script
                      << " — is the Quorum repo intact?\n";
            return 1;
        }
    }

    // 4. Shell out. std::system so the (fast) readout streams live. Quote args.
    std::string cmd = "python3 \"" + script + "\" --project \"" + project_root +
                      "\" --since \"" + since + "\"";
    if (!opts.until.empty()) cmd += " --until \"" + opts.until + "\"";
    if (opts.json) cmd += " --json";
    int status = std::system(cmd.c_str());
    int exit_code = (status != -1 && WIFEXITED(status))
                        ? WEXITSTATUS(status) : -1;
    return exit_code;
}

}  // namespace sui::quorum::cli
