// tests/unit/test_agent_create.cpp
// Unit tests for the agent create CLI scaffolding (offline path only — no claude -p).
//
// Run:  cd build && cmake .. && make test_agent_create && ./test_agent_create

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

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
        ("quorum_test_agent_" + std::to_string(getpid()) + "_" + std::to_string(g_test_num++));
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

// --- Test A: Offline scaffolding creates correct files ----------------------

static void test_offline_scaffolding() {
    std::cout << "\n=== A. Offline scaffolding creates correct files ===\n\n";

    auto tmp = make_temp_dir();
    auto data_dir = tmp + "/data";

    // Run from the temp dir so configs/agents/ is created inside it
    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    sui::quorum::cli::AgentCreateParams p;
    p.role = "thinker";
    p.name = "test-agent";
    p.project = "test-proj";
    p.data_dir = data_dir;
    p.no_ai = true;  // template won't exist, so minimal fallback

    int rc = sui::quorum::cli::create_agent(p);

    check(rc == 0, "A: create_agent returns 0");
    check(fs::exists("configs/agents/test-proj/test-agent.yaml"),
          "A: YAML config file exists");
    check(fs::exists(data_dir + "/vaults/test-agent"),
          "A: vault directory exists");
    check(fs::exists(data_dir + "/vaults/test-agent/knowledge"),
          "A: knowledge/ subdirectory exists");
    check(fs::exists(data_dir + "/vaults/test-agent/CONTEXT.md"),
          "A: CONTEXT.md exists");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test B: Role validation rejects invalid roles --------------------------

static void test_invalid_role() {
    std::cout << "\n=== B. Role validation rejects invalid roles ===\n\n";

    auto tmp = make_temp_dir();
    auto data_dir = tmp + "/data";

    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    sui::quorum::cli::AgentCreateParams p;
    p.role = "invalid";
    p.name = "bad-agent";
    p.project = "test-proj";
    p.data_dir = data_dir;
    p.no_ai = true;

    int rc = sui::quorum::cli::create_agent(p);

    check(rc == 1, "B: create_agent returns 1 for invalid role");
    check(!fs::exists("configs/agents/test-proj/bad-agent.yaml"),
          "B: no config file created");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test C: Duplicate agent prevention -------------------------------------

static void test_duplicate_agent() {
    std::cout << "\n=== C. Duplicate agent prevention ===\n\n";

    auto tmp = make_temp_dir();
    auto data_dir = tmp + "/data";

    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    sui::quorum::cli::AgentCreateParams p;
    p.role = "thinker";
    p.name = "dup-agent";
    p.project = "test-proj";
    p.data_dir = data_dir;
    p.no_ai = true;

    int rc1 = sui::quorum::cli::create_agent(p);
    check(rc1 == 0, "C: first create_agent returns 0");

    int rc2 = sui::quorum::cli::create_agent(p);
    check(rc2 == 1, "C: second create_agent returns 1 (duplicate)");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test D: Doer role includes executor section in YAML --------------------

static void test_doer_executor_section() {
    std::cout << "\n=== D. Doer role includes executor section in YAML ===\n\n";

    auto tmp = make_temp_dir();
    auto data_dir = tmp + "/data";

    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    sui::quorum::cli::AgentCreateParams p;
    p.role = "doer";
    p.name = "doer-agent";
    p.project = "test-proj";
    p.data_dir = data_dir;
    p.target_dir = "/some/path";
    p.no_ai = true;

    int rc = sui::quorum::cli::create_agent(p);
    check(rc == 0, "D: create_agent returns 0");

    auto yaml = read_file("configs/agents/test-proj/doer-agent.yaml");
    check(yaml.find("executor:") != std::string::npos,
          "D: YAML contains 'executor:'");
    check(yaml.find("target_dir:") != std::string::npos,
          "D: YAML contains 'target_dir:'");
    check(yaml.find("allowed_tools:") != std::string::npos,
          "D: YAML contains 'allowed_tools:'");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test E: Non-doer role excludes executor section ------------------------

static void test_non_doer_no_executor() {
    std::cout << "\n=== E. Non-doer role excludes executor section ===\n\n";

    auto tmp = make_temp_dir();
    auto data_dir = tmp + "/data";

    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    sui::quorum::cli::AgentCreateParams p;
    p.role = "thinker";
    p.name = "think-agent";
    p.project = "test-proj";
    p.data_dir = data_dir;
    p.no_ai = true;

    int rc = sui::quorum::cli::create_agent(p);
    check(rc == 0, "E: create_agent returns 0");

    auto yaml = read_file("configs/agents/test-proj/think-agent.yaml");
    check(yaml.find("executor:") == std::string::npos,
          "E: YAML does NOT contain 'executor:'");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test F: Skill file included in YAML when provided ----------------------

static void test_skill_file_in_yaml() {
    std::cout << "\n=== F. Skill file included in YAML when provided ===\n\n";

    auto tmp = make_temp_dir();
    auto data_dir = tmp + "/data";

    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    sui::quorum::cli::AgentCreateParams p;
    p.role = "scribe";
    p.name = "skill-agent";
    p.project = "test-proj";
    p.data_dir = data_dir;
    p.skill_file = "path/to/SKILL.md";
    p.no_ai = true;

    int rc = sui::quorum::cli::create_agent(p);
    check(rc == 0, "F: create_agent returns 0");

    auto yaml = read_file("configs/agents/test-proj/skill-agent.yaml");
    check(yaml.find("skill_file: path/to/SKILL.md") != std::string::npos,
          "F: YAML contains 'skill_file: path/to/SKILL.md'");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- main -------------------------------------------------------------------

int main() {
    std::cout << "=== Agent Create Tests ===\n";

    test_offline_scaffolding();
    test_invalid_role();
    test_duplicate_agent();
    test_doer_executor_section();
    test_non_doer_no_executor();
    test_skill_file_in_yaml();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
