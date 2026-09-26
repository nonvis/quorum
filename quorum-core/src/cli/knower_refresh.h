#pragma once

// Phase 14 Track 3 — `quorum knower refresh [--knower <name>] [--all]
//                     [--project <path|name>]`.
//
// Re-runs the read-only "knower" Tier-2 scan pass(es) that re-survey the live
// codebase and SELF-WRITE the knower vault artifacts (knowledge/ref-*.md). This
// is the SAME work `scripts/run-knower.sh <project> <knower>` already does — a
// single-knower `converse --mode brainstorm` scan that emits the ref artifact
// and HANDOFFs done. Knowers are the sole accumulators (Decision #46); generic
// and autopilot modes accumulate by REFRESHING the affected knowers after the
// doer ships.
//
// IMPLEMENTATION CHOICE — SHELL OUT to scripts/run-knower.sh (not reimplement):
//   - run-knower.sh already owns the canonical {knower -> goal, budget, artifact}
//     map (lines 46-71). Reimplementing it here would DUPLICATE the goal strings
//     and let them drift. Shelling out keeps ONE source of truth.
//   - The script is reachable robustly: the `quorum` CLI binary lives at
//     <repo>/build/quorum_daemon and `make install` symlinks ~/.local/bin/quorum
//     -> that binary, so fs::canonical(argv[0]) resolves THROUGH the symlink back
//     into <repo>; <repo>/scripts/run-knower.sh is then always findable. This is
//     the same self-relative resolution `init` uses (main.cpp ~:679).
//
// The PURE parts (arg parsing, knower-name validation, the ordered --all list,
// project resolution via cli/ask.h's resolve_project_path, and the missing-setup
// precondition check) live in helpers below and are unit-tested directly
// (tests/unit/test_knower_refresh.cpp). The only process I/O that spends tokens
// is the run-knower.sh shell-out inside run_knower_refresh.
//
// ONE LENS AT A TIME. A project runs one daemon (the pid lock in main.cpp), so
// lenses cannot refresh concurrently: `--parallel` used to launch three tracks,
// and the second and third could only seed their conversation into the running
// daemon's queue and exit — reported as FAILED while the daemon still served
// them (Decision #87, 2026-09-26). The flag is still accepted, and ignored with a
// notice. For the same reason a refresh refuses to start while another daemon
// runs in the project: its `converse` would only queue behind that daemon.
//
// Header-only, matches the cli/ask.h convention.

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <signal.h>     // kill(pid, 0) — is the project's daemon alive?
#include <sys/wait.h>   // WIFEXITED / WEXITSTATUS

#include "cli/ask.h"            // resolve_project_path (reused, not reimplemented)
#include "utils/config.h"       // load_config — the project's daemon.pid_file

namespace sui::quorum::cli {

struct KnowerRefreshOptions {
    std::string project;        // path OR project name; empty = cwd (".")
    std::string knower;         // one of the valid names; empty unless --knower
    bool all = false;           // --all: refresh every knower in dependency order
    bool parallel = false;      // --parallel: accepted and IGNORED (Decision #87)
    // Resolved during run: the repo root that contains scripts/run-knower.sh.
    // Set from argv[0] in main.cpp (fs::canonical self-resolution); empty = let
    // run-knower.sh resolution fall back to a best-effort sibling lookup.
    std::string quorum_root;
};

namespace knower_refresh_detail {

// The valid knower names, in the dependency-sensible refresh order used by
// --all: cartographer first (produces the layout index), then architect (which
// READS the cartographer index), then historian, then recap. PURE.
[[nodiscard]] inline const std::vector<std::string>& ordered_knowers() {
    static const std::vector<std::string> k = {
        "cartographer", "architect", "historian", "recap"};
    return k;
}

// Is `name` a valid knower? PURE.
[[nodiscard]] inline bool is_valid_knower(const std::string& name) {
    for (const auto& k : ordered_knowers())
        if (k == name) return true;
    return false;
}

// "cartographer | architect | historian | recap" for error messages. PURE.
[[nodiscard]] inline std::string valid_knowers_list() {
    std::string s;
    for (const auto& k : ordered_knowers()) {
        if (!s.empty()) s += " | ";
        s += k;
    }
    return s;
}

// The required Tier-1 input that must already exist for a knower to refresh
// (proof that setup-knowers.sh ran). Relative to <project_root>. PURE.
//   - cartographer / architect : .quorum/cartographer/layout.json
//       (architect READS the cartographer index, so it shares the precondition)
//   - historian                : .quorum/historian/decisions-raw.json
//   - recap                    : .quorum/recap/timeline-raw.json
[[nodiscard]] inline std::string required_input_rel(const std::string& knower) {
    if (knower == "cartographer" || knower == "architect")
        return ".quorum/cartographer/layout.json";
    if (knower == "historian")
        return ".quorum/historian/decisions-raw.json";
    if (knower == "recap")
        return ".quorum/recap/timeline-raw.json";
    return {};
}

// Precondition check: the knower's agent yaml AND its required Tier-1 input must
// both exist under <project_root>. On failure, set `err` to a clear message that
// tells the operator to run setup-knowers.sh, and return false. PURE
// (filesystem reads only). `ok==true` means the knower is set up to refresh.
[[nodiscard]] inline bool knower_is_setup(const std::string& project_root,
                                          const std::string& knower,
                                          std::string& err) {
    namespace fs = std::filesystem;
    err.clear();
    fs::path root(project_root);

    auto agent_yaml = root / ".quorum" / "agents" / (knower + ".yaml");
    std::error_code ec;
    if (!fs::exists(agent_yaml, ec)) {
        err = "knower '" + knower + "' is not set up (missing " +
              agent_yaml.string() +
              ") — run scripts/setup-knowers.sh <project> first";
        return false;
    }

    auto rel = required_input_rel(knower);
    if (!rel.empty()) {
        auto input = root / rel;
        std::error_code iec;
        if (!fs::exists(input, iec)) {
            err = "knower '" + knower + "' is missing its Tier-1 input (" +
                  input.string() +
                  ") — run scripts/setup-knowers.sh <project> first";
            return false;
        }
    }
    return true;
}

// Resolve <quorum_root>/scripts/run-knower.sh. If quorum_root is empty, returns
// "scripts/run-knower.sh" (relative) as a last-resort fallback. PURE.
[[nodiscard]] inline std::string run_knower_script(
    const std::string& quorum_root) {
    namespace fs = std::filesystem;
    if (quorum_root.empty()) return "scripts/run-knower.sh";
    return (fs::path(quorum_root) / "scripts" / "run-knower.sh").string();
}

// The pid of a daemon running in this project, if any. Reads the project's
// daemon.pid_file (`.quorum/quorum.pid` as init writes it; a relative path is
// project-rooted) and asks the OS whether that pid is alive. A missing or stale
// pid file means none. Filesystem + kill(pid, 0) only.
[[nodiscard]] inline std::optional<long> running_daemon_pid(
    const std::string& project_root) {
    namespace fs = std::filesystem;
    fs::path pid_file = fs::path(project_root) / ".quorum" / "quorum.pid";
    auto cfg = sui::quorum::load_config(
        (fs::path(project_root) / ".quorum" / "config.yaml").string());
    if (cfg && !cfg->daemon.pid_file.empty()) {
        fs::path p(cfg->daemon.pid_file);
        pid_file = p.is_relative() ? fs::path(project_root) / p : p;
    }
    std::ifstream in(pid_file);
    long pid = 0;
    if (!(in >> pid) || pid <= 0) return std::nullopt;
    if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) return std::nullopt;
    return pid;
}

}  // namespace knower_refresh_detail

// Top-level entrypoint for `quorum knower refresh`. Resolves the project root
// (reusing cli/ask.h's resolve_project_path), validates the requested knower(s),
// checks each is set up, then shells out to scripts/run-knower.sh per knower in
// dependency order. Returns 0 iff every requested refresh succeeded; 1 on any
// resolution / validation / setup / refresh failure (failures print BEFORE or
// per-knower during the run).
[[nodiscard]] inline int run_knower_refresh(const KnowerRefreshOptions& opts) {
    namespace fs = std::filesystem;
    using namespace knower_refresh_detail;

    // 0. Require exactly one of --all / --knower.
    if (!opts.all && opts.knower.empty()) {
        std::cerr << "ERROR: knower refresh requires --all or --knower <name>\n"
                     "Usage: quorum knower refresh [--all | --knower <"
                  << valid_knowers_list()
                  << ">] [--project <path|name>]\n";
        return 1;
    }
    if (opts.all && !opts.knower.empty()) {
        std::cerr << "ERROR: --all and --knower are mutually exclusive\n";
        return 1;
    }

    // 1. Validate the single-knower name BEFORE resolving the project (cheap,
    //    deterministic). On a bad name, list the valid ones.
    if (!opts.all && !is_valid_knower(opts.knower)) {
        std::cerr << "ERROR: unknown knower '" << opts.knower
                  << "' (valid: " << valid_knowers_list() << ")\n";
        return 1;
    }

    // 1b. --parallel is accepted and ignored (Decision #87).
    if (opts.parallel) {
        std::cout << "note: --parallel is ignored — a project runs one daemon, so "
                     "lenses refresh one after another\n";
    }

    // 2. Resolve project root (default cwd) via the SAME helper `quorum ask`
    //    uses — searches ~/projects/<name> then ~/nonvis/<name> for a name arg.
    std::string arg = opts.project.empty() ? std::string(".") : opts.project;
    std::string err;
    std::string project_root = resolve_project_path(arg, err);
    if (project_root.empty()) {
        std::cerr << "ERROR: " << err << "\n";
        return 1;
    }

    // 3. Build the ordered list of knowers to refresh.
    std::vector<std::string> targets;
    if (opts.all) {
        targets = ordered_knowers();
    } else {
        targets.push_back(opts.knower);
    }

    // 4. Precondition: every target must be set up. Check ALL up front so we
    //    don't spend tokens on knower 1 then abort on knower 2's missing setup.
    for (const auto& k : targets) {
        std::string serr;
        if (!knower_is_setup(project_root, k, serr)) {
            std::cerr << "ERROR: " << serr << "\n";
            return 1;
        }
    }

    // 4b. Refuse while another daemon runs in this project: each pass starts its
    //     own `converse`, which would only queue behind it and exit at once.
    if (auto pid = running_daemon_pid(project_root)) {
        std::cerr << "ERROR: a Quorum daemon is already running in " << project_root
                  << " (pid " << *pid << ") — let it finish or stop it, then "
                     "refresh\n";
        return 1;
    }

    // 5. Shell out to run-knower.sh per knower, in order. Each pass spends tokens
    //    (it runs `quorum converse --mode brainstorm`). Stop on the first failure.
    auto script = run_knower_script(opts.quorum_root);
    {
        std::error_code ec;
        if (!fs::exists(script, ec)) {
            std::cerr << "ERROR: run-knower.sh not found at " << script
                      << " — is the Quorum repo intact?\n";
            return 1;
        }
    }

    // 6. std::system live-streams each pass to the operator's terminal. Stop on
    //    the first failure (ordered).
    int failures = 0;
    for (const auto& k : targets) {
        std::cout << "=== refreshing knower: " << k << " (project: "
                  << project_root << ") ===\n";
        std::cout.flush();
        // Quote both args; run-knower.sh re-resolves the project to an absolute
        // path itself. Use std::system (NOT run_command/popen) so the multi-
        // minute converse pass streams its progress LIVE to the operator's
        // terminal instead of being buffered into a string and dumped at the end.
        std::string cmd = "\"" + script + "\" \"" + project_root + "\" " + k;
        int status = std::system(cmd.c_str());
        int exit_code = (status != -1 && WIFEXITED(status))
                            ? WEXITSTATUS(status) : -1;
        if (exit_code != 0) {
            std::cerr << "ERROR: refresh failed for knower '" << k
                      << "' (exit " << exit_code << ")\n";
            ++failures;
            break;  // ordered: a failed cartographer would starve the architect
        }
    }

    if (failures > 0) {
        std::cerr << "knower refresh: " << failures
                  << " knower(s) failed to refresh\n";
        return 1;
    }
    std::cout << "knower refresh: " << targets.size()
              << " knower(s) refreshed\n";
    return 0;
}

}  // namespace sui::quorum::cli
