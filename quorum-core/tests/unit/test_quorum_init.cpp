// tests/unit/test_quorum_init.cpp
// Unit tests for quorum init and .quorum/ detection in agent create.
//
// Run:  cd build && cmake .. && make test_quorum_init && ./test_quorum_init

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "cli/init.h"
#include "cli/agent_create.h"

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
        ("quorum_test_init_" + std::to_string(getpid()) + "_" + std::to_string(g_test_num++));
    fs::create_directories(dir);
    return dir.string();
}

static void cleanup_temp(const std::string& path) {
    fs::remove_all(path);
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// --- Test A: init creates directory structure --------------------------------

static void test_init_creates_structure() {
    std::cout << "\n=== A. init creates directory structure ===\n\n";

    auto tmp = make_temp_dir();
    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    int rc = sui::quorum::cli::init_project();

    check(rc == 0, "A: init_project returns 0");
    check(fs::exists(".quorum"), "A: .quorum/ exists");
    check(fs::exists(".quorum/agents"), "A: .quorum/agents/ exists");
    check(fs::exists(".quorum/vaults/leader/knowledge"), "A: .quorum/vaults/leader/knowledge/ exists");
    check(fs::exists(".quorum/teams"), "A: .quorum/teams/ exists");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test B: init creates config.yaml with correct content ------------------

static void test_init_creates_config() {
    std::cout << "\n=== B. init creates config.yaml ===\n\n";

    auto tmp = make_temp_dir();
    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    sui::quorum::cli::init_project();

    check(fs::exists(".quorum/config.yaml"), "B: .quorum/config.yaml exists");

    auto content = read_file(".quorum/config.yaml");
    check(content.find("leader: leader") != std::string::npos,
          "B: config contains 'leader: leader'");
    check(content.find("hourly_limit_usd") != std::string::npos,
          "B: config contains 'hourly_limit_usd'");
    check(content.find("- config: .quorum/agents/leader.yaml") != std::string::npos,
          "B: config contains '- config: .quorum/agents/leader.yaml'");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test C: init creates leader agent files --------------------------------

static void test_init_creates_leader() {
    std::cout << "\n=== C. init creates leader agent files ===\n\n";

    auto tmp = make_temp_dir();
    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    sui::quorum::cli::init_project();

    check(fs::exists(".quorum/agents/leader.yaml"), "C: leader.yaml exists");

    auto yaml = read_file(".quorum/agents/leader.yaml");
    check(yaml.find("id: leader") != std::string::npos,
          "C: leader.yaml contains 'id: leader'");
    check(yaml.find("role: leader") != std::string::npos,
          "C: leader.yaml contains 'role: leader'");

    check(fs::exists(".quorum/vaults/leader/CONTEXT.md"), "C: CONTEXT.md exists");

    auto ctx = read_file(".quorum/vaults/leader/CONTEXT.md");
    check(ctx.find("Leader") != std::string::npos,
          "C: CONTEXT.md contains 'Leader'");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test D: init refuses double init ---------------------------------------

static void test_init_refuses_double_init() {
    std::cout << "\n=== D. init refuses double init ===\n\n";

    auto tmp = make_temp_dir();
    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    int rc1 = sui::quorum::cli::init_project();
    check(rc1 == 0, "D: first init returns 0");

    int rc2 = sui::quorum::cli::init_project();
    check(rc2 == 1, "D: second init returns 1");

    // Verify files not corrupted
    auto content = read_file(".quorum/config.yaml");
    check(!content.empty(), "D: config.yaml still readable after double init");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test E: init creates .gitignore ----------------------------------------

static void test_init_creates_gitignore() {
    std::cout << "\n=== E. init creates .gitignore ===\n\n";

    auto tmp = make_temp_dir();
    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    sui::quorum::cli::init_project();

    check(fs::exists(".quorum/.gitignore"), "E: .gitignore exists");

    auto content = read_file(".quorum/.gitignore");
    check(content.find("quorum.db") != std::string::npos,
          "E: .gitignore contains 'quorum.db'");
    check(content.find("vaults/*/knowledge/") != std::string::npos,
          "E: .gitignore contains 'vaults/*/knowledge/'");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test F: agent create detects .quorum/ ----------------------------------

static void test_agent_create_detects_quorum_dir() {
    std::cout << "\n=== F. agent create detects .quorum/ ===\n\n";

    auto tmp = make_temp_dir();
    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    // First init
    sui::quorum::cli::init_project();

    // Create a doer agent
    sui::quorum::cli::AgentCreateParams p;
    p.role = "doer";
    p.name = "test-dev";
    p.project = ".";
    p.data_dir = tmp + "/data";
    p.no_ai = true;

    int rc = sui::quorum::cli::create_agent(p);
    check(rc == 0, "F: create_agent returns 0");

    check(fs::exists(".quorum/agents/test-dev.yaml"),
          "F: .quorum/agents/test-dev.yaml exists (NOT configs/agents/)");
    check(fs::exists(".quorum/vaults/test-dev"),
          "F: .quorum/vaults/test-dev/ exists");
    check(fs::exists(".quorum/vaults/test-dev/knowledge"),
          "F: .quorum/vaults/test-dev/knowledge/ exists");

    auto config = read_file(".quorum/config.yaml");
    check(config.find("- config: .quorum/agents/test-dev.yaml") != std::string::npos,
          "F: config.yaml contains '- config: .quorum/agents/test-dev.yaml'");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- main -------------------------------------------------------------------

int main() {
    std::cout << "=== Quorum Init Tests ===\n";

    test_init_creates_structure();
    test_init_creates_config();
    test_init_creates_leader();
    test_init_refuses_double_init();
    test_init_creates_gitignore();
    test_agent_create_detects_quorum_dir();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
