// tests/unit/test_auto_commit.cpp
// Findings F6/F4 — the daemon-side completion auto-commit
// (daemon/conversation.h :: auto_commit_on_completion) is SCOPED to `.quorum/**`.
//
// The bug it fixes: a whole-tree `git add -A && git commit` swept the entire
// uncommitted working tree (multiple unrelated tasks) into one commit labeled as
// a knower refresh, authored as the user, in the 2026-07-21 Crucible dogfood.
// The fix is a PATHSPEC commit (`git add -- .quorum && git commit -- .quorum`):
// it commits ONLY `.quorum` paths and leaves any foreign pre-staged index entry
// staged and untouched.
//
// These cases run against a REAL temp git repo (std::filesystem temp dir +
// `git init`), driving the SAME run_command helper the function itself uses.
//
// Cases:
//   1. `.quorum` change + unrelated untracked file + a PRE-STAGED foreign file:
//      returns true; HEAD lists ONLY the .quorum path; the foreign file is still
//      staged (A) and the unrelated file is still untracked (??).
//   2. clean `.quorum` (dirty tree elsewhere): returns false, no new commit.
//   3. non-git directory: returns false.
//   4. empty dir strings: returns false.
//
// Run:  cd build && ctest -R test_auto_commit --output-on-failure

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "daemon/conversation.h"   // auto_commit_on_completion
#include "utils/subprocess.h"      // run_command

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

// Run a git (or shell) command inside `dir`, discarding output.
static void run_in(const fs::path& dir, const std::string& cmd) {
    (void)sui::quorum::run_command(
        "cd \"" + dir.string() + "\" && " + cmd + " >/dev/null 2>&1");
}

// Run a command inside `dir`, returning captured stdout.
static std::string out_in(const fs::path& dir, const std::string& cmd) {
    auto r = sui::quorum::run_command(
        "cd \"" + dir.string() + "\" && " + cmd + " 2>/dev/null");
    return r ? r->output : std::string();
}

// Fresh temp dir per case so they can't collide.
static fs::path make_dir(const std::string& tag) {
    auto base = fs::temp_directory_path() /
        ("quorum_auto_commit_test_" + tag + "_" + std::to_string(::getpid()));
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

// Init a git repo with a local identity (no reliance on global config) and
// gpg-signing disabled so `git commit` cannot block.
static void git_init(const fs::path& dir) {
    run_in(dir, "git init");
    run_in(dir, "git config user.email test@quorum.local");
    run_in(dir, "git config user.name TestBot");
    run_in(dir, "git config commit.gpgsign false");
}

static int commit_count(const fs::path& dir) {
    auto s = out_in(dir, "git rev-list --count HEAD");
    try { return std::stoi(s); } catch (...) { return -1; }
}

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// ---- Case 1: scoped commit — foreign pre-staged index left untouched --------
static void test_1_scoped_commit() {
    std::cout << "\n=== Case 1: scoped .quorum commit, foreign index untouched ===\n\n";

    auto repo = make_dir("1");
    git_init(repo);

    // Our bookkeeping change under .quorum/ (untracked).
    write_file(repo / ".quorum" / "vaults" / "x" / "note.md", "hi\n");
    // Unrelated untracked file outside .quorum.
    write_file(repo / "src" / "other.txt", "unrelated\n");
    // A foreign file the human/another session PRE-STAGED before the call.
    write_file(repo / "src" / "staged.txt", "foreign staged\n");
    run_in(repo, "git add src/staged.txt");

    bool ok = sui::quorum::auto_commit_on_completion(
        7, /*target_dir=*/"", /*project_root=*/repo.string(), "my goal");
    check(ok, "1: returns true when there is a .quorum change");

    // HEAD must list ONLY the .quorum path.
    auto names = out_in(repo, "git show --name-only --format= HEAD");
    check(contains(names, ".quorum/vaults/x/note.md"),
          "1: HEAD includes the .quorum path");
    check(!contains(names, "src/staged.txt"),
          "1: HEAD does NOT include the foreign pre-staged file");
    check(!contains(names, "src/other.txt"),
          "1: HEAD does NOT include the unrelated untracked file");

    // The foreign file is still staged; the unrelated file is still untracked.
    auto status = out_in(repo, "git status --porcelain");
    check(contains(status, "A  src/staged.txt"),
          "1: foreign file still staged (A) after the commit");
    check(contains(status, "?? src/other.txt"),
          "1: unrelated file still untracked (?\?)");
    check(!contains(status, ".quorum/vaults/x/note.md"),
          "1: our .quorum change is committed (no longer dirty)");
}

// ---- Case 2: clean .quorum, dirty tree elsewhere — no-op --------------------
static void test_2_clean_quorum() {
    std::cout << "\n=== Case 2: clean .quorum (dirty elsewhere) → no-op ===\n\n";

    auto repo = make_dir("2");
    git_init(repo);
    // Commit an initial state that INCLUDES a clean .quorum so HEAD exists and
    // .quorum is tracked-and-clean.
    write_file(repo / ".quorum" / "config.md", "cfg\n");
    write_file(repo / "README.md", "readme\n");
    run_in(repo, "git add -A");
    run_in(repo, "git commit -m init");

    int before = commit_count(repo);
    // Dirty the tree OUTSIDE .quorum only.
    write_file(repo / "src" / "dirty.txt", "dirty\n");

    bool ok = sui::quorum::auto_commit_on_completion(
        9, "", repo.string(), "goal");
    check(!ok, "2: returns false when nothing under .quorum changed");
    check(commit_count(repo) == before, "2: no new commit created");
}

// ---- Case 3: non-git directory — no-op --------------------------------------
static void test_3_non_git() {
    std::cout << "\n=== Case 3: non-git directory → no-op ===\n\n";

    auto dir = make_dir("3");   // plain dir, NO git init
    write_file(dir / ".quorum" / "x.md", "x\n");

    bool ok = sui::quorum::auto_commit_on_completion(
        1, "", dir.string(), "goal");
    check(!ok, "3: returns false in a non-git directory");
}

// ---- Case 4: empty dir strings — no-op --------------------------------------
static void test_4_empty_dirs() {
    std::cout << "\n=== Case 4: empty dir strings → no-op ===\n\n";

    check(!sui::quorum::auto_commit_on_completion(1, "", "", "goal"),
          "4: returns false when both target_dir and project_root are empty");
}

// ---- main ------------------------------------------------------------------
int main() {
    std::cout << "=== auto_commit_on_completion Unit Tests ===\n";

    test_1_scoped_commit();
    test_2_clean_quorum();
    test_3_non_git();
    test_4_empty_dirs();

    // Best-effort cleanup of the per-case temp dirs.
    std::error_code ec;
    for (const char* tag : {"1", "2", "3"}) {
        fs::remove_all(fs::temp_directory_path() /
            ("quorum_auto_commit_test_" + std::string(tag) + "_" +
             std::to_string(::getpid())), ec);
    }

    std::cout << "\nResults: " << g_passed << " passed, " << g_failed
              << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
