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
// ⚠️ PARALLEL MODE (--parallel, OPT-IN, --all only): `--all` can refresh the
// INDEPENDENT lenses concurrently (see parallel_tracks below). This path is GATED
// and default-OFF because parallel `converse` instances SHARE the one
// <project>/.quorum/quorum.db in WAL mode (a quorum.db-wal is present) —
// concurrent writers there can hit SQLite lock contention / "database is locked".
// Do NOT flip the default to parallel until the validation gate in
// docs/proposals/knower-refresh-scaling.md passes (WAL proven sufficient across
// repeated real runs, OR per-invocation db isolation, OR a write-serializing
// advisory lock). Serial (std::system, live-streamed) stays the default UX.
//
// Header-only, matches the cli/ask.h convention.

#include <cstdlib>
#include <filesystem>
#include <future>       // std::async / std::future (parallel path)
#include <iostream>
#include <mutex>        // std::mutex — serialize per-lens output blocks
#include <string>
#include <vector>

#include <sys/wait.h>   // WIFEXITED / WEXITSTATUS

#include "cli/ask.h"            // resolve_project_path (reused, not reimplemented)
#include "utils/subprocess.h"   // run_command — capture per-lens output in parallel

namespace sui::quorum::cli {

struct KnowerRefreshOptions {
    std::string project;        // path OR project name; empty = cwd (".")
    std::string knower;         // one of the valid names; empty unless --knower
    bool all = false;           // --all: refresh every knower in dependency order
    bool parallel = false;      // --parallel (--all only, OPT-IN): run independent
                                // tracks concurrently. GATED — see header ⚠️ note.
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

// Group the refresh `targets` into concurrency TRACKS: lenses WITHIN a track run
// SEQUENTIALLY (a track is a dependency chain), and tracks run CONCURRENTLY. PURE.
//
// WHY cartographer and architect share ONE track (a real dependency edge, not a
// cosmetic ordering): the architect SKILL opportunistically reads cartographer's
// *Tier-2* output. templates/skills/architect/SKILL.md:17 ("Read those first")
// directs architect to read `.quorum/cartographer/layout.json` AND, when present,
// cartographer's annotated `ref-project-index.md` as its component inventory. A
// fresh cartographer Tier-2 pass therefore improves architect's input, so
// architect must run AFTER cartographer — same track, cartographer first. The
// other two lenses read only their OWN Tier-1 inputs (historian:
// decisions-raw.json, recap: timeline-raw.json), so each is an independent
// single-lens track that can run fully in parallel.
//
// Contract:
//   - cartographer AND architect both in targets -> one track
//     {cartographer, architect} (cartographer first).
//   - a LONE architect (cartographer NOT in targets) -> its own single-lens track.
//   - every other knower -> its own single-lens track.
//   - relative order follows ordered_knowers().
// For the full --all list this yields {{cartographer, architect}, {historian},
// {recap}} — three tracks, so wall time = max(carto+arch, historian, recap), NOT
// a 4-way flatten (the carto->arch chain forbids that).
[[nodiscard]] inline std::vector<std::vector<std::string>> parallel_tracks(
    const std::vector<std::string>& targets) {
    auto in_targets = [&](const std::string& name) {
        for (const auto& t : targets)
            if (t == name) return true;
        return false;
    };
    const bool has_carto = in_targets("cartographer");

    std::vector<std::vector<std::string>> tracks;
    // Iterate the canonical order so relative order is preserved and cartographer
    // is always processed (its track created) before architect.
    for (const auto& k : ordered_knowers()) {
        if (!in_targets(k)) continue;
        if (k == "architect" && has_carto) {
            // Chain architect onto cartographer's (already-created) track.
            for (auto& track : tracks) {
                if (!track.empty() && track.front() == "cartographer") {
                    track.push_back("architect");
                    break;
                }
            }
        } else {
            tracks.push_back({k});
        }
    }
    return tracks;
}

// One lens's outcome in a parallel refresh run. PURE data.
//   exit_code == 0 && !skipped -> refreshed
//   exit_code != 0 && !skipped -> failed (ran, nonzero exit)
//   skipped                    -> never ran (an earlier lens in ITS track failed)
struct LensResult {
    std::string knower;
    int exit_code = 0;
    bool skipped = false;
};

// The reduced tally of a parallel run. PURE data.
struct RefreshTally {
    int failed_count = 0;
    int skipped_count = 0;
    int refreshed_count = 0;
};

// Reduce per-lens results to counts. PURE — the parallel path's summary math, so
// it is unit-testable without spawning anything.
[[nodiscard]] inline RefreshTally summarize_results(
    const std::vector<LensResult>& results) {
    RefreshTally t;
    for (const auto& r : results) {
        if (r.skipped) ++t.skipped_count;
        else if (r.exit_code != 0) ++t.failed_count;
        else ++t.refreshed_count;
    }
    return t;
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

    // 1b. --parallel is --all-only. Reject it early (BEFORE project resolution,
    //     so this needs no fixture project) — parallelism only makes sense across
    //     the multi-lens --all set.
    if (opts.parallel && !opts.all) {
        std::cerr << "ERROR: --parallel requires --all\n";
        return 1;
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

    // 5p. PARALLEL path (opt-in, --all only — enforced at step 1b). Run each TRACK
    //     concurrently; within a track lenses run SEQUENTIALLY (cartographer ->
    //     architect). A lens failure skips only the REMAINING lenses of ITS OWN
    //     track — other tracks run to completion (a historian failure must NOT
    //     skip recap).
    //
    //     OUTPUT DISCIPLINE: three concurrent std::system calls would interleave
    //     their line-buffered progress into garbage. So in parallel mode we
    //     CAPTURE each lens's combined stdout+stderr with run_command (popen) and
    //     print it as ONE block when that lens finishes, under a header, guarded
    //     by a std::mutex. TRADE-OFF: this forgoes the LIVE streaming that serial
    //     std::system gives the operator — which is exactly WHY serial stays the
    //     default UX. stdout here is in lens-COMPLETION order, not launch order.
    if (opts.parallel) {
        auto tracks = parallel_tracks(targets);

        std::cout << "knower refresh: launching " << tracks.size()
                  << " track(s) in parallel (project: " << project_root
                  << "):\n";
        for (const auto& track : tracks) {
            std::cout << "  - ";
            for (size_t i = 0; i < track.size(); ++i)
                std::cout << (i ? " -> " : "") << track[i];
            std::cout << "\n";
        }
        std::cout.flush();

        std::mutex print_mu;
        auto quote = [](const std::string& s) { return "\"" + s + "\""; };

        // Run one track sequentially; return a LensResult per lens in it. Captures
        // by ref are safe: every future is .get()'d before this function returns.
        auto run_track =
            [&](const std::vector<std::string>& track)
            -> std::vector<LensResult> {
            std::vector<LensResult> out;
            bool track_failed = false;
            for (const auto& k : track) {
                if (track_failed) {
                    // An earlier lens in THIS track failed -> skip the rest of it.
                    out.push_back(LensResult{k, 0, /*skipped=*/true});
                    std::lock_guard<std::mutex> lk(print_mu);
                    std::cout << "=== knower " << k
                              << " skipped (upstream lens in its track failed) "
                                 "===\n";
                    std::cout.flush();
                    continue;
                }
                // 2>&1: fold stderr into the captured stream so nothing is lost
                // (run_command reads stdout only). run-knower.sh re-resolves the
                // project to an absolute path itself.
                std::string cmd = quote(script) + " " + quote(project_root) +
                                  " " + k + " 2>&1";
                auto res = sui::quorum::run_command(cmd);
                int exit_code = res ? res->exit_code : -1;
                {
                    std::lock_guard<std::mutex> lk(print_mu);
                    std::cout << "=== knower " << k << " finished (exit "
                              << exit_code << ") ===\n";
                    if (res) std::cout << res->output;
                    std::cout.flush();
                }
                out.push_back(LensResult{k, exit_code, /*skipped=*/false});
                if (exit_code != 0) track_failed = true;
            }
            return out;
        };

        // Launch every track concurrently, then collect (blocks per track).
        std::vector<std::future<std::vector<LensResult>>> futs;
        futs.reserve(tracks.size());
        for (const auto& track : tracks)
            futs.push_back(std::async(std::launch::async, run_track, track));

        std::vector<LensResult> results;
        for (auto& f : futs) {
            auto track_res = f.get();
            for (auto& r : track_res) results.push_back(std::move(r));
        }

        // End summary: per-lens one-liner, then the existing-style final line.
        auto tally = summarize_results(results);
        std::cout << "\n=== knower refresh summary ===\n";
        for (const auto& r : results) {
            if (r.skipped)
                std::cout << "  " << r.knower << ": skipped\n";
            else if (r.exit_code != 0)
                std::cout << "  " << r.knower << ": FAILED (exit "
                          << r.exit_code << ")\n";
            else
                std::cout << "  " << r.knower << ": refreshed\n";
        }
        // Retry hint: the exact single-lens command per failed/skipped lens.
        if (tally.failed_count + tally.skipped_count > 0) {
            std::cout << "retry the un-refreshed lens(es):\n";
            for (const auto& r : results) {
                if (r.skipped || r.exit_code != 0)
                    std::cout << "  quorum knower refresh --knower " << r.knower
                              << " --project " << project_root << "\n";
            }
            std::cerr << "knower refresh: " << tally.failed_count
                      << " knower(s) failed, " << tally.skipped_count
                      << " skipped\n";
            return 1;
        }
        std::cout << "knower refresh: " << tally.refreshed_count
                  << " knower(s) refreshed\n";
        return 0;
    }

    // 5s. SERIAL path (default): std::system live-streams each pass to the
    //     operator's terminal. Stop on the first failure (ordered).
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
