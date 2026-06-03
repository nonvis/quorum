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

    std::error_code ec;
    fs::remove_all(tdir, ec);

    std::cout << "\nResults: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
