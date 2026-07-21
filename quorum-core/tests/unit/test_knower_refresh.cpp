// tests/unit/test_knower_refresh.cpp
// Phase 14 Track 3 — `quorum knower refresh` CLI. PURE parts only — NO live
// run-knower.sh / claude shell-out.
//
// Assertions:
//   (A) knower-name validation:
//       (a) is_valid_knower accepts each of the four lenses, rejects junk
//       (b) ordered_knowers() is exactly [cartographer, architect, historian,
//           recap] in that dependency order (architect reads cartographer)
//       (c) valid_knowers_list() lists all four for error messages
//   (B) required_input_rel — the per-knower Tier-1 precondition path:
//       cartographer/architect -> cartographer/layout.json; historian ->
//       historian/decisions-raw.json; recap -> recap/timeline-raw.json
//   (C) knower_is_setup — precondition gate:
//       (a) missing agent yaml -> false + err names setup-knowers.sh
//       (b) agent yaml present but Tier-1 input missing -> false + err
//       (c) both present -> true, err empty
//   (D) project-resolution reuse (resolve_project_path from cli/ask.h):
//       a dir WITH .quorum/ resolves; one WITHOUT fails (same helper `ask` uses)
//   (E) run_knower_script: <root>/scripts/run-knower.sh; empty root -> relative
//   (F) generic_refresh_recommendation (conversation.h, pure string):
//       contains the conv id, "quorum knower refresh", "--all", the project,
//       and the named-knower variant
//   (G) parallel_tracks — the --parallel concurrency grouping:
//       full --all -> {{cartographer,architect},{historian},{recap}};
//       lone architect -> own track; {historian,recap} -> two singletons;
//       {cartographer,architect} -> one track, cartographer first
//   (H) summarize_results — the parallel per-lens tally:
//       all-pass -> 0 failed/0 skipped; historian-failed + recap-refreshed
//       (no cross-track skip); cartographer-failed + architect-skipped -> 1/1
//   (I) --parallel requires --all: run_knower_refresh({parallel, !all, recap})
//       returns 1 BEFORE project resolution (needs no fixture project)
//
// Run:  cd build && ctest -R test_knower_refresh --output-on-failure

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "cli/knower_refresh.h"
#include "daemon/conversation.h"   // generic_refresh_recommendation

namespace fs = std::filesystem;
namespace kd = sui::quorum::cli::knower_refresh_detail;

static int g_passed = 0;
static int g_failed = 0;

#define check(cond, msg) do {                                         \
    if (cond) { ++g_passed; std::cout << "  PASS: " << msg << "\n"; } \
    else      { ++g_failed; std::cerr << "  FAIL: " << msg << "\n"; } \
} while (0)

static void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::trunc);
    f << content;
}

// ---- Case A: knower-name validation ---------------------------------------
static void test_A_validation() {
    std::cout << "\n=== Case A: knower-name validation ===\n\n";

    check(kd::is_valid_knower("cartographer"), "A(a): cartographer valid");
    check(kd::is_valid_knower("architect"),    "A(a): architect valid");
    check(kd::is_valid_knower("historian"),    "A(a): historian valid");
    check(kd::is_valid_knower("recap"),        "A(a): recap valid");
    check(!kd::is_valid_knower("scribe"),      "A(a): scribe rejected");
    check(!kd::is_valid_knower(""),            "A(a): empty rejected");
    check(!kd::is_valid_knower("Cartographer"),"A(a): case-sensitive reject");

    const auto& order = kd::ordered_knowers();
    check(order.size() == 4, "A(b): four knowers");
    check(order.size() == 4 && order[0] == "cartographer" &&
              order[1] == "architect" && order[2] == "historian" &&
              order[3] == "recap",
          "A(b): order is cartographer->architect->historian->recap");

    auto list = kd::valid_knowers_list();
    check(list.find("cartographer") != std::string::npos &&
              list.find("architect") != std::string::npos &&
              list.find("historian") != std::string::npos &&
              list.find("recap") != std::string::npos,
          "A(c): valid_knowers_list lists all four");
}

// ---- Case B: required_input_rel -------------------------------------------
static void test_B_required_input() {
    std::cout << "\n=== Case B: required_input_rel ===\n\n";

    check(kd::required_input_rel("cartographer") ==
              ".quorum/cartographer/layout.json",
          "B: cartographer -> cartographer/layout.json");
    check(kd::required_input_rel("architect") ==
              ".quorum/cartographer/layout.json",
          "B: architect shares the cartographer precondition");
    check(kd::required_input_rel("historian") ==
              ".quorum/historian/decisions-raw.json",
          "B: historian -> historian/decisions-raw.json");
    check(kd::required_input_rel("recap") ==
              ".quorum/recap/timeline-raw.json",
          "B: recap -> recap/timeline-raw.json");
}

// ---- Case C: knower_is_setup ----------------------------------------------
static void test_C_setup_gate(const fs::path& tdir) {
    std::cout << "\n=== Case C: knower_is_setup ===\n\n";

    // (a) Bare project (.quorum/ but no agent yaml) -> not set up.
    auto bare = tdir / "C_bare";
    fs::create_directories(bare / ".quorum");
    {
        std::string err;
        bool ok = kd::knower_is_setup(bare.string(), "cartographer", err);
        check(!ok, "C(a): missing agent yaml -> not set up");
        check(err.find("setup-knowers.sh") != std::string::npos,
              "C(a): err points to setup-knowers.sh");
    }

    // (b) Agent yaml present but Tier-1 input missing -> not set up.
    auto half = tdir / "C_half";
    write_file(half / ".quorum" / "agents" / "cartographer.yaml",
               "name: \"cartographer\"\nrole: thinker\n");
    {
        std::string err;
        bool ok = kd::knower_is_setup(half.string(), "cartographer", err);
        check(!ok, "C(b): missing Tier-1 input -> not set up");
        check(err.find("layout.json") != std::string::npos,
              "C(b): err names the missing layout.json input");
    }

    // (c) Both present -> set up.
    auto full = tdir / "C_full";
    write_file(full / ".quorum" / "agents" / "historian.yaml",
               "name: \"historian\"\nrole: thinker\n");
    write_file(full / ".quorum" / "historian" / "decisions-raw.json", "[]\n");
    {
        std::string err;
        bool ok = kd::knower_is_setup(full.string(), "historian", err);
        check(ok, "C(c): agent yaml + Tier-1 input present -> set up");
        check(err.empty(), "C(c): no error when set up");
    }
}

// ---- Case D: project-resolution reuse -------------------------------------
static void test_D_project_resolution(const fs::path& tdir) {
    std::cout << "\n=== Case D: resolve_project_path reuse ===\n\n";

    auto with_q = tdir / "D_with_quorum";
    fs::create_directories(with_q / ".quorum");
    {
        std::string err;
        auto resolved =
            sui::quorum::cli::resolve_project_path(with_q.string(), err);
        check(!resolved.empty() && err.empty(),
              "D: dir with .quorum/ resolves (same helper as `ask`)");
    }

    auto no_q = tdir / "D_no_quorum";
    fs::create_directories(no_q);
    {
        std::string err;
        auto resolved =
            sui::quorum::cli::resolve_project_path(no_q.string(), err);
        check(resolved.empty() && !err.empty(),
              "D: dir without .quorum/ fails resolution");
    }
}

// ---- Case E: run_knower_script --------------------------------------------
static void test_E_script_path() {
    std::cout << "\n=== Case E: run_knower_script ===\n\n";

    check(kd::run_knower_script("/opt/quorum") ==
              "/opt/quorum/scripts/run-knower.sh",
          "E: <root>/scripts/run-knower.sh when root set");
    check(kd::run_knower_script("") == "scripts/run-knower.sh",
          "E: relative fallback when root empty");
}

// ---- Case F: generic_refresh_recommendation -------------------------------
static void test_F_recommendation() {
    std::cout << "\n=== Case F: generic_refresh_recommendation ===\n\n";

    auto msg = sui::quorum::generic_refresh_recommendation(42, "/home/u/proj");
    check(msg.find("conversation 42") != std::string::npos,
          "F: includes the conversation id");
    check(msg.find("quorum knower refresh") != std::string::npos,
          "F: includes the `quorum knower refresh` command");
    check(msg.find("--all") != std::string::npos, "F: includes --all");
    check(msg.find("/home/u/proj") != std::string::npos,
          "F: includes the project path");
    check(msg.find("--knower") != std::string::npos,
          "F: offers the single-lens --knower variant");

    // Empty/'.' project degrades to a <project> placeholder (no bare cwd path).
    auto msg2 = sui::quorum::generic_refresh_recommendation(7, ".");
    check(msg2.find("<project>") != std::string::npos,
          "F: '.' project degrades to <project> placeholder");
}

// ---- Case G: parallel_tracks ----------------------------------------------
static void test_G_parallel_tracks() {
    std::cout << "\n=== Case G: parallel_tracks ===\n\n";

    // Full --all list -> {{cartographer, architect}, {historian}, {recap}}.
    // Three tracks (NOT a 4-way flatten): the carto->arch Tier-2 chain forbids it.
    {
        auto tracks = kd::parallel_tracks(kd::ordered_knowers());
        bool shape =
            tracks.size() == 3 &&
            tracks[0].size() == 2 && tracks[0][0] == "cartographer" &&
                tracks[0][1] == "architect" &&
            tracks[1].size() == 1 && tracks[1][0] == "historian" &&
            tracks[2].size() == 1 && tracks[2][0] == "recap";
        check(shape,
              "G: --all -> {{cartographer,architect},{historian},{recap}}");
    }

    // Lone architect (cartographer NOT in targets) -> its own single-lens track.
    {
        auto tracks = kd::parallel_tracks({"architect"});
        check(tracks.size() == 1 && tracks[0].size() == 1 &&
                  tracks[0][0] == "architect",
              "G: {architect} alone -> {{architect}} (own track)");
    }

    // Two independent lenses -> two singleton tracks (canonical order preserved).
    {
        auto tracks = kd::parallel_tracks({"historian", "recap"});
        check(tracks.size() == 2 &&
                  tracks[0].size() == 1 && tracks[0][0] == "historian" &&
                  tracks[1].size() == 1 && tracks[1][0] == "recap",
              "G: {historian,recap} -> two singleton tracks");
    }

    // cartographer + architect -> one track, cartographer first.
    {
        auto tracks = kd::parallel_tracks({"cartographer", "architect"});
        check(tracks.size() == 1 && tracks[0].size() == 2 &&
                  tracks[0][0] == "cartographer" && tracks[0][1] == "architect",
              "G: {cartographer,architect} -> one track, cartographer first");
    }
}

// ---- Case H: summarize_results --------------------------------------------
static void test_H_summarize_results() {
    std::cout << "\n=== Case H: summarize_results ===\n\n";

    // All pass -> 0 failed, 0 skipped, all refreshed.
    {
        std::vector<kd::LensResult> r = {
            {"cartographer", 0, false}, {"architect", 0, false},
            {"historian", 0, false},    {"recap", 0, false}};
        auto t = kd::summarize_results(r);
        check(t.failed_count == 0 && t.skipped_count == 0 &&
                  t.refreshed_count == 4,
              "H: all-pass -> 0 failed / 0 skipped / 4 refreshed");
    }

    // historian FAILED but recap refreshed -> the no-cross-track-skip contract is
    // representable: a failure in one track leaves the other track's lens intact.
    {
        std::vector<kd::LensResult> r = {
            {"historian", 1, false}, {"recap", 0, false}};
        auto t = kd::summarize_results(r);
        check(t.failed_count == 1 && t.skipped_count == 0 &&
                  t.refreshed_count == 1,
              "H: historian FAILED + recap refreshed (no cross-track skip)");
    }

    // cartographer FAILED -> architect skipped (same track): 1 failed, 1 skipped.
    {
        std::vector<kd::LensResult> r = {
            {"cartographer", 1, false}, {"architect", 0, true}};
        auto t = kd::summarize_results(r);
        check(t.failed_count == 1 && t.skipped_count == 1 &&
                  t.refreshed_count == 0,
              "H: cartographer FAILED -> architect skipped (1 failed/1 skipped)");
    }
}

// ---- Case I: --parallel requires --all (early exit, no fixture) -----------
static void test_I_flag_validation() {
    std::cout << "\n=== Case I: --parallel flag validation ===\n\n";

    // parallel=true, all=false, knower="recap": must return 1 BEFORE any project
    // resolution (so no fixture project is needed). Verifies the step-1b guard
    // fires ahead of resolve_project_path. Does NOT execute run-knower.sh.
    sui::quorum::cli::KnowerRefreshOptions opts;
    opts.parallel = true;
    opts.all = false;
    opts.knower = "recap";
    int rc = sui::quorum::cli::run_knower_refresh(opts);
    check(rc == 1, "I: --parallel without --all returns 1 (pre-resolution)");
}

// ---- main ------------------------------------------------------------------
int main() {
    auto tdir = fs::temp_directory_path() /
                ("quorum-test-knower-refresh-" + std::to_string(::getpid()));
    fs::create_directories(tdir);

    test_A_validation();
    test_B_required_input();
    test_C_setup_gate(tdir);
    test_D_project_resolution(tdir);
    test_E_script_path();
    test_F_recommendation();
    test_G_parallel_tracks();
    test_H_summarize_results();
    test_I_flag_validation();

    std::error_code ec;
    fs::remove_all(tdir, ec);

    std::cout << "\nResults: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
