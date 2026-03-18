// tests/unit/test_discover.cpp
// Unit tests for config auto-discovery (.quorum/ walk-up).
//
// Run:  cd build && cmake .. && make test_discover && ./test_discover

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "utils/discover.h"
#include "cli/init.h"

namespace fs = std::filesystem;

// --- helpers ----------------------------------------------------------------

static int g_passed = 0;
static int g_failed = 0;
static int g_test_num = 0;

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        ++g_failed;
        std::exit(1);
    }
    std::cout << "[PASS] " << msg << "\n";
    ++g_passed;
}

static std::string make_temp_dir() {
    auto dir = fs::temp_directory_path() /
        ("quorum_test_discover_" + std::to_string(getpid()) + "_" + std::to_string(g_test_num++));
    fs::create_directories(dir);
    return dir.string();
}

static void cleanup_temp(const std::string& path) {
    fs::remove_all(path);
}

// --- Test A: discover from project root --------------------------------------

static void test_discover_from_project_root() {
    std::cout << "\n=== A. discover from project root ===\n\n";

    auto tmp = make_temp_dir();
    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    int rc = sui::quorum::cli::init_project();
    check(rc == 0, "A: init_project returns 0");

    auto result = sui::quorum::discover_config(tmp);
    check(result.has_value(), "A: discover_config returns a value");
    check(result->find(".quorum/config.yaml") != std::string::npos,
          "A: returned path ends with .quorum/config.yaml");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test B: discover from subdirectory --------------------------------------

static void test_discover_from_subdirectory() {
    std::cout << "\n=== B. discover from subdirectory ===\n\n";

    auto tmp = make_temp_dir();
    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    sui::quorum::cli::init_project();

    auto sub = tmp + "/src";
    fs::create_directories(sub);

    auto result = sui::quorum::discover_config(sub);
    check(result.has_value(), "B: discover_config returns a value from src/");
    check(result->find(".quorum/config.yaml") != std::string::npos,
          "B: returned path contains .quorum/config.yaml");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test C: discover from nested subdirectory -------------------------------

static void test_discover_from_nested_subdirectory() {
    std::cout << "\n=== C. discover from nested subdirectory ===\n\n";

    auto tmp = make_temp_dir();
    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    sui::quorum::cli::init_project();

    auto nested = tmp + "/src/components";
    fs::create_directories(nested);

    auto result = sui::quorum::discover_config(nested);
    check(result.has_value(), "C: discover_config returns a value from src/components/");
    check(result->find(".quorum/config.yaml") != std::string::npos,
          "C: returned path contains .quorum/config.yaml");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test D: discover not found ----------------------------------------------

static void test_discover_not_found() {
    std::cout << "\n=== D. discover not found ===\n\n";

    auto tmp = make_temp_dir();
    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    // No init — empty directory
    auto result = sui::quorum::discover_config(tmp);
    check(!result.has_value(), "D: discover_config returns nullopt for empty dir");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test E: discover project root -------------------------------------------

static void test_discover_project_root() {
    std::cout << "\n=== E. discover project root ===\n\n";

    auto tmp = make_temp_dir();
    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    sui::quorum::cli::init_project();

    auto sub = tmp + "/src";
    fs::create_directories(sub);

    auto result = sui::quorum::discover_project_root(sub);
    check(result.has_value(), "E: discover_project_root returns a value");

    // Returned path should be the project root, not the subdirectory
    auto canonical_tmp = fs::canonical(tmp).string();
    auto canonical_result = fs::canonical(*result).string();
    check(canonical_result == canonical_tmp,
          "E: returned path is the project root (not src/)");
    check(result->find("/src") == std::string::npos || result->back() != 'c',
          "E: returned path does NOT end with src");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test F: max_depth respected ---------------------------------------------

static void test_max_depth_respected() {
    std::cout << "\n=== F. max_depth respected ===\n\n";

    auto tmp = make_temp_dir();
    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    sui::quorum::cli::init_project();

    auto deep = tmp + "/a/b/c";
    fs::create_directories(deep);

    // max_depth=1: can only check deep and deep/.. = tmp/a/b — not enough
    auto result1 = sui::quorum::discover_config(deep, 1);
    check(!result1.has_value(), "F: discover_config returns nullopt with max_depth=1");

    // max_depth=5: sufficient depth to reach project root
    auto result2 = sui::quorum::discover_config(deep, 5);
    check(result2.has_value(), "F: discover_config returns value with max_depth=5");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test G: returned path is absolute ---------------------------------------

static void test_returned_path_is_absolute() {
    std::cout << "\n=== G. returned path is absolute ===\n\n";

    auto tmp = make_temp_dir();
    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    sui::quorum::cli::init_project();

    auto config_result = sui::quorum::discover_config(tmp);
    check(config_result.has_value(), "G: discover_config returns a value");
    check(config_result->front() == '/', "G: config path starts with / (absolute)");

    auto root_result = sui::quorum::discover_project_root(tmp);
    check(root_result.has_value(), "G: discover_project_root returns a value");
    check(root_result->front() == '/', "G: project root path starts with / (absolute)");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- main -------------------------------------------------------------------

int main() {
    std::cout << "=== Config Discovery Tests ===\n";

    test_discover_from_project_root();
    test_discover_from_subdirectory();
    test_discover_from_nested_subdirectory();
    test_discover_not_found();
    test_discover_project_root();
    test_max_depth_respected();
    test_returned_path_is_absolute();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
