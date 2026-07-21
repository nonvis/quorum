// tests/unit/test_spend.cpp
// Per-run spend readout (`quorum spend`) — PURE parts only. NO python shell-out,
// NO claude. Exercises the unit-testable helpers in cli/spend.h + the run_spend
// precondition (no --since AND no LOCK -> error before any subprocess).
//
// Assertions:
//   (A) transcript_dir_for — the munge rule ('/' and '.' -> '-'):
//       (a) a normal abs path -> <HOME>/.claude/projects/-Users-...-crucible
//       (b) a path containing a '.' munges the dot too
//   (B) lock_started_at — the --since fallback (LOCK line 1):
//       (a) LOCK whose line 1 is an ISO timestamp -> returns it
//       (b) LOCK missing -> ""
//       (c) LOCK whose line 1 is garbage (not ISO-shaped) -> ""
//   (C) run_spend — no --since and no LOCK in a resolvable project -> returns 1
//       (must NOT invoke python: the error is returned before the shell-out)
//
// Run:  cd build && ctest -R test_spend --output-on-failure

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "cli/spend.h"

namespace fs = std::filesystem;
namespace sd = sui::quorum::cli::spend_detail;

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

static fs::path make_dir(const std::string& tag) {
    auto base = fs::temp_directory_path() /
        ("quorum_spend_test_" + tag + "_" + std::to_string(::getpid()));
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

// ---- Case A: transcript_dir_for (the munge rule) ---------------------------
static void test_A_transcript_dir() {
    std::cout << "\n=== Case A: transcript_dir_for munge rule ===\n\n";

    ::setenv("HOME", "/home/testuser", 1);

    check(sd::transcript_dir_for("/Users/sangsoo/nonvis/crucible") ==
              "/home/testuser/.claude/projects/-Users-sangsoo-nonvis-crucible",
          "A(a): normal abs path munges every '/' to '-'");

    check(sd::transcript_dir_for("/Users/x/my.app/proj") ==
              "/home/testuser/.claude/projects/-Users-x-my-app-proj",
          "A(b): a '.' in the path is munged too");
}

// ---- Case B: lock_started_at (the --since fallback) ------------------------
static void test_B_lock_started_at() {
    std::cout << "\n=== Case B: lock_started_at ===\n\n";

    // (a) LOCK line 1 = ISO timestamp -> returned verbatim.
    {
        auto proj = make_dir("B_ok");
        write_file(proj / ".quorum" / "autopilot" / "LOCK",
                   "2026-07-21T10:00:00Z\n"
                   "supervisor session live — no external git in this repo\n");
        check(sd::lock_started_at(proj.string()) == "2026-07-21T10:00:00Z",
              "B(a): ISO line 1 returned");
    }

    // (b) no LOCK file -> "".
    {
        auto proj = make_dir("B_missing");
        fs::create_directories(proj / ".quorum" / "autopilot");
        check(sd::lock_started_at(proj.string()).empty(),
              "B(b): missing LOCK -> empty");
    }

    // (c) LOCK line 1 is garbage (not ISO-shaped) -> "".
    {
        auto proj = make_dir("B_garbage");
        write_file(proj / ".quorum" / "autopilot" / "LOCK",
                   "not a timestamp\n2026-07-21T10:00:00Z\n");
        check(sd::lock_started_at(proj.string()).empty(),
              "B(c): non-ISO line 1 -> empty");
    }
}

// ---- Case C: run_spend precondition (no since, no LOCK -> 1, no python) -----
static void test_C_run_spend_no_since() {
    std::cout << "\n=== Case C: run_spend with no --since and no LOCK -> 1 ===\n\n";

    // A resolvable project (has .quorum/) but NO LOCK and NO --since: run_spend
    // must return 1 at the since-resolution step, BEFORE any python shell-out.
    auto proj = make_dir("C_proj");
    fs::create_directories(proj / ".quorum");   // makes it resolvable

    sui::quorum::cli::SpendOptions opts;
    opts.project = proj.string();               // absolute -> resolved as-is
    // opts.since empty, no LOCK present.
    opts.quorum_root = "/nonexistent-quorum-root";  // proves we never reach the
                                                    // script-locate / shell-out

    int rc = sui::quorum::cli::run_spend(opts);
    check(rc == 1, "C: returns 1 when no --since and no LOCK");
}

int main() {
    std::cout << "=== spend readout Unit Tests ===\n";

    test_A_transcript_dir();
    test_B_lock_started_at();
    test_C_run_spend_no_since();

    // Best-effort cleanup of the per-case temp dirs.
    std::error_code ec;
    for (const char* tag : {"B_ok", "B_missing", "B_garbage", "C_proj"}) {
        fs::remove_all(fs::temp_directory_path() /
            ("quorum_spend_test_" + std::string(tag) + "_" +
             std::to_string(::getpid())), ec);
    }

    std::cout << "\nResults: " << g_passed << " passed, " << g_failed
              << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
