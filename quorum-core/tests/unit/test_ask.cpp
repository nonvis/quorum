// tests/unit/test_ask.cpp
// Phase 12 — `quorum ask` CLI. PURE helpers only (resolve_project_path +
// assemble_manager_prompt). NO live claude call.
//
// Assertions:
//   (A) resolve_project_path:
//       (a) an existing dir containing .quorum/ resolves to itself
//       (b) a dir WITHOUT .quorum/ returns "" with a non-empty err
//       (c) name-convention: with HOME pointed at a temp dir and
//           <tmpHOME>/nonvis/foo/.quorum present, resolving "foo" finds it
//   (B) assemble_manager_prompt:
//       (a) the prompt contains the question
//       (b) seeded Pitch/00 - Introduction.md + 00 - Decision Log.md content
//           appears in the prompt
//       (c) with NO curated files present it still returns a non-empty prompt
//           containing the question (graceful degrade), no crash
//
// Run:  cd build && ctest -R test_ask --output-on-failure

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "cli/ask.h"

namespace fs = std::filesystem;

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

// ---- Case A: resolve_project_path -----------------------------------------
static void test_A_resolve(const fs::path& tdir) {
    std::cout << "\n=== Case A: resolve_project_path ===\n\n";

    // (a) existing dir WITH .quorum/ resolves to itself.
    auto with_q = tdir / "A_with_quorum";
    fs::create_directories(with_q / ".quorum");
    {
        std::string err;
        auto resolved = sui::quorum::cli::resolve_project_path(with_q.string(),
                                                               err);
        check(!resolved.empty(), "A(a): dir with .quorum/ resolves non-empty");
        check(err.empty(), "A(a): no error on success");
        // Resolved path points at the same directory (compare canonical forms).
        std::error_code ec;
        check(fs::equivalent(resolved, with_q, ec),
              "A(a): resolved path equivalent to the input dir");
    }

    // (b) existing dir WITHOUT .quorum/ -> "" + non-empty err.
    auto no_q = tdir / "A_no_quorum";
    fs::create_directories(no_q);
    {
        std::string err;
        auto resolved = sui::quorum::cli::resolve_project_path(no_q.string(),
                                                               err);
        check(resolved.empty(), "A(b): dir without .quorum/ returns \"\"");
        check(!err.empty(), "A(b): err is non-empty");
        check(err.find("no .quorum/ found") != std::string::npos,
              "A(b): err mentions 'no .quorum/ found'");
    }

    // (c) name-convention: HOME -> temp, <HOME>/nonvis/foo/.quorum present.
    {
        auto fake_home = tdir / "A_home";
        auto foo_quorum = fake_home / "nonvis" / "foo" / ".quorum";
        fs::create_directories(foo_quorum);

        const char* saved_home = std::getenv("HOME");
        std::string saved = saved_home ? saved_home : "";
        setenv("HOME", fake_home.string().c_str(), 1);

        std::string err;
        auto resolved = sui::quorum::cli::resolve_project_path("foo", err);

        // Restore HOME before asserting (so a failure doesn't leak state).
        if (saved_home) setenv("HOME", saved.c_str(), 1);
        else unsetenv("HOME");

        check(!resolved.empty(),
              "A(c): name 'foo' resolves via <HOME>/nonvis/foo");
        check(err.empty(), "A(c): no error for name-convention resolve");
        auto expected = (fake_home / "nonvis" / "foo").string();
        check(resolved == expected,
              "A(c): resolved path == <HOME>/nonvis/foo");
    }
}

// ---- Case B: assemble_manager_prompt --------------------------------------
static void test_B_assemble(const fs::path& tdir) {
    std::cout << "\n=== Case B: assemble_manager_prompt ===\n\n";

    const std::string question =
        "What is the project's stance on durable independent storage?";

    // (a)+(b): seeded curated layer.
    {
        auto proj = tdir / "B_seeded";
        fs::create_directories(proj / ".quorum");
        write_file(proj / "Pitch" / "00 - Introduction.md",
                   "# Pitch\n\n## What we're building\n\n"
                   "PITCH_INTRO_MARKER: provenance-on-Sui protocol.\n");
        write_file(proj / "00 - Decision Log.md",
                   "# Decision Log\n\n### 2026-05-19 — Provenance is the moat\n\n"
                   "DECISION_LOG_MARKER: durable-storage claim dropped.\n");

        auto prompt = sui::quorum::cli::assemble_manager_prompt(proj.string(),
                                                                question);
        check(!prompt.empty(), "B(a): prompt non-empty");
        check(prompt.find(question) != std::string::npos,
              "B(a): prompt contains the question");
        check(prompt.find("PITCH_INTRO_MARKER: provenance-on-Sui protocol.")
                  != std::string::npos,
              "B(b): Pitch/Introduction content appears in prompt");
        check(prompt.find("DECISION_LOG_MARKER: durable-storage claim dropped.")
                  != std::string::npos,
              "B(b): Decision Log content appears in prompt");
        // Manager persona present.
        check(prompt.find("You are the manager of this project")
                  != std::string::npos,
              "B(b): manager persona present");
    }

    // (c): NO curated files present -> still non-empty, contains question.
    {
        auto proj = tdir / "B_empty";
        fs::create_directories(proj / ".quorum");  // .quorum exists; no curated files

        auto prompt = sui::quorum::cli::assemble_manager_prompt(proj.string(),
                                                                question);
        check(!prompt.empty(), "B(c): prompt non-empty with no curated files");
        check(prompt.find(question) != std::string::npos,
              "B(c): prompt still contains the question (graceful degrade)");
        // Graceful-degrade markers for missing curated files.
        check(prompt.find("(not present)") != std::string::npos,
              "B(c): missing curated files marked '(not present)'");
        check(prompt.find("(no learnings.md yet)") != std::string::npos,
              "B(c): missing learnings marked '(no learnings.md yet)'");
    }
}

// ---- main ------------------------------------------------------------------
int main() {
    auto tdir = fs::temp_directory_path() /
                ("quorum-test-ask-" + std::to_string(::getpid()));
    fs::create_directories(tdir);

    test_A_resolve(tdir);
    test_B_assemble(tdir);

    std::error_code ec;
    fs::remove_all(tdir, ec);

    std::cout << "\nResults: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
