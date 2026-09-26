// tests/unit/test_temp_file.cpp
// Unit tests for utils/temp_file.h — the private temp files Quorum hands to
// `claude -p`. Unique mkstemp names with the owner's pid (task ids repeat
// across project databases, so the old /tmp/quorum_prompt_<task_id>.txt let
// concurrent projects overwrite each other's prompt), mode 0600, removed on
// scope exit; the startup sweep removes only files whose owner is gone.
//
// Run:  cd build && ctest -R test_temp_file

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "utils/temp_file.h"

namespace fs = std::filesystem;
using sui::quorum::TempFile;

static int g_passed = 0;

static void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        std::exit(1);
    }
    std::cout << "[PASS] " << msg << "\n";
    ++g_passed;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static void touch(const fs::path& p) { std::ofstream(p) << "x"; }

static void test_create_and_scope() {
    std::cout << "\n=== A. create: private, unique, removed on scope exit ===\n\n";
    std::string path;
    {
        auto a = TempFile::create("prompt", "hello prompt");
        auto b = TempFile::create("prompt", "second");
        path = a.path();
        check(!path.empty() && fs::exists(path), "A: file created");
        check(read_file(path) == "hello prompt", "A: holds the content");
        check(a.path() != b.path(), "A: two files get two names");
        const auto name = fs::path(path).filename().string();
        check(name.starts_with("quorum-" + std::to_string(::getpid()) + "-prompt-"),
              "A: name carries the owner's pid and the tag");
        struct stat st{};
        check(::stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600,
              "A: mode 0600 (not world-readable)");

        TempFile moved = std::move(a);
        check(moved.path() == path && a.path().empty(), "A: move transfers ownership");
    }
    check(!fs::exists(path), "A: removed when it goes out of scope");

    auto c = TempFile::create("ask", "");
    auto cpath = c.path();
    check(!cpath.empty() && fs::exists(cpath) && read_file(cpath).empty(),
          "A: empty content is fine");
    c.remove();
    check(!fs::exists(cpath) && c.path().empty(), "A: remove() deletes it early");
}

static void test_sweep() {
    std::cout << "\n=== B. sweep: removes only files whose owner is gone ===\n\n";
    auto dir = fs::temp_directory_path() / ("quorum_test_sweep_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);

    // A pid that is certainly gone: a child that has exited and been reaped.
    pid_t child = ::fork();
    if (child == 0) ::_exit(0);
    ::waitpid(child, nullptr, 0);

    const auto dead = dir / ("quorum-" + std::to_string(child) + "-prompt-abc123");
    const auto live = dir / ("quorum-" + std::to_string(::getpid()) + "-prompt-def456");
    const auto old_legacy = dir / "quorum_prompt_7.txt";
    const auto new_legacy = dir / "quorum_sysprompt_8.txt";
    const auto unrelated = dir / "quorum-bench-XYZ123";
    const auto smoke_dir = dir / "quorum-smoke.AbC123";
    for (const auto& p : {dead, live, old_legacy, new_legacy, unrelated}) touch(p);
    fs::create_directories(smoke_dir);
    fs::last_write_time(old_legacy, fs::file_time_type::clock::now() - std::chrono::hours(48));

    int removed = sui::quorum::sweep_stale_temp_files(dir);
    check(removed == 2, "B: two files removed (" + std::to_string(removed) + ")");
    check(!fs::exists(dead), "B: a dead owner's file is removed");
    check(fs::exists(live), "B: a live owner's file is kept");
    check(!fs::exists(old_legacy), "B: an old-style file older than a day is removed");
    check(fs::exists(new_legacy), "B: a recent old-style file is kept (may be in use)");
    check(fs::exists(unrelated) && fs::exists(smoke_dir),
          "B: other quorum-* names (benchmark files, smoke dirs) are left alone");
    fs::remove_all(dir);
}

int main() {
    test_create_and_scope();
    test_sweep();
    std::cout << "\n" << g_passed << " checks passed\n";
    return 0;
}
