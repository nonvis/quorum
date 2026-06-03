// tests/unit/test_self_path.cpp
// Unit tests for utils/self_path.h — the portable self-executable-path helper
// that fixes init/knower-refresh/benchmark repo-root resolution under the
// installed `quorum` PATH symlink (argv[0] == bare "quorum").
//
// Run:  cd build && cmake .. && make test_self_path && ./test_self_path

#include <filesystem>
#include <iostream>
#include <string>

#include "utils/self_path.h"

namespace fs = std::filesystem;

static int g_passed = 0;
static int g_failed = 0;

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        ++g_failed;
        std::exit(1);
    }
    std::cout << "[PASS] " << msg << "\n";
    ++g_passed;
}

int main() {
    std::cout << "\n=== self_executable_path() ===\n\n";

    // On macOS (_NSGetExecutablePath) and Linux (/proc/self/exe) this MUST
    // resolve to a non-empty, existing, absolute path — the test binary itself.
    auto self = sui::quorum::self_executable_path();

#if defined(__APPLE__) || defined(__linux__)
    check(!self.empty(), "self path is non-empty on macOS/Linux");
    check(fs::path(self).is_absolute(), "self path is absolute");
    check(fs::exists(self), "self path points at an existing file");
    // The basename should be this test binary.
    check(fs::path(self).filename() == "test_self_path",
          "self path basename == test_self_path");
    // canonical() means no symlink components remain.
    check(fs::canonical(self) == fs::path(self),
          "self path is already canonical");
#else
    check(self.empty(), "self path is empty on unsupported platform");
#endif

    // quorum_repo_root_from_exe: with a real exe, the helper-first branch wins,
    // so it returns <exe>/../.. regardless of argv0. Here the test binary lives
    // at <build>/test_self_path, so the derived "root" is <build>/.. — just
    // assert it's a non-empty, existing directory (we don't depend on layout).
    auto root = sui::quorum::quorum_repo_root_from_exe("test_self_path");
    check(!root.empty(), "repo-root-from-exe is non-empty");
    check(fs::is_directory(root), "repo-root-from-exe is an existing directory");

    // nullptr argv0 must not crash; with a real exe it still resolves via the
    // helper-first branch.
    auto root_null = sui::quorum::quorum_repo_root_from_exe(nullptr);
    check(!root_null.empty(), "repo-root-from-exe(nullptr) is non-empty");

    std::cout << "\n" << g_passed << " passed, " << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
