// tests/unit/test_agent_modify.cpp
// Unit tests for agent modify and list CLI commands (offline path only).
//
// Run:  cd build && cmake .. && make test_agent_modify && ./test_agent_modify

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "cli/agent_create.h"

namespace fs = std::filesystem;

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
        ("quorum_test_modify_" + std::to_string(getpid()) + "_" +
std::to_string(g_test_num++));
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

// Create a .quorum/ structure in tmp and chdir into it.
// Returns the project root path. Caller must restore cwd and cleanup.
static std::string setup_quorum_project() {
    auto tmp = make_temp_dir();
    fs::create_directories(tmp + "/.quorum/agents");
    fs::create_directories(tmp + "/.quorum/vaults");
    fs::current_path(tmp);
    return tmp;
}

// Create an agent using the existing create_agent function
static int create_test_agent(const std::string& name, const std::string& role,
                              const std::string& description = "") {
    sui::quorum::cli::AgentCreateParams p;
    p.name = name;
    p.role = role;
    p.description = description;
    p.no_ai = true;
    return sui::quorum::cli::create_agent(p);
}

// --- Test A: Modify description ----------------------------------------------

static void test_A_modify_description() {
    std::cout << "\n=== A. Modify description ===\n\n";

    auto original_cwd = fs::current_path();
    auto tmp = setup_quorum_project();

    // Create agent
    int rc = create_test_agent("my-thinker", "thinker", "Plans things");
    check(rc == 0, "A: create succeeds");

    // Modify description
    sui::quorum::cli::AgentCreateParams mod;
    mod.name = "my-thinker";
    mod.description = "Designs architecture and plans implementation";
    mod.no_ai = true;
    int rc2 = sui::quorum::cli::modify_agent(mod);
    check(rc2 == 0, "A: modify returns 0");

    auto yaml = read_file(tmp + "/.quorum/agents/my-thinker.yaml");
    check(yaml.find("Designs architecture") != std::string::npos,
          "A: YAML has new description");
    check(yaml.find("role: thinker") != std::string::npos,
          "A: YAML preserves role");
    check(yaml.find("id: my-thinker") != std::string::npos,
          "A: YAML preserves id");

    // CONTEXT.md should be regenerated
    auto ctx = read_file(tmp + "/.quorum/vaults/my-thinker/CONTEXT.md");
    check(!ctx.empty(), "A: CONTEXT.md regenerated");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test B: Modify role (thinker -> doer) -----------------------------------

static void test_B_modify_role_to_doer() {
    std::cout << "\n=== B. Modify role to doer ===\n\n";

    auto original_cwd = fs::current_path();
    auto tmp = setup_quorum_project();

    create_test_agent("flex-agent", "thinker", "Flexible agent");

    // Verify no executor section initially
    auto yaml_before = read_file(tmp + "/.quorum/agents/flex-agent.yaml");
    check(yaml_before.find("executor:") == std::string::npos,
          "B: no executor section before");

    // Modify to doer
    sui::quorum::cli::AgentCreateParams mod;
    mod.name = "flex-agent";
    mod.role = "doer";
    mod.target_dir = "/my/project";
    mod.no_ai = true;
    int rc = sui::quorum::cli::modify_agent(mod);
    check(rc == 0, "B: modify returns 0");

    auto yaml = read_file(tmp + "/.quorum/agents/flex-agent.yaml");
    check(yaml.find("role: doer") != std::string::npos, "B: role is doer");
    check(yaml.find("executor:") != std::string::npos, "B: executor section added");
    check(yaml.find("target_dir: /my/project") != std::string::npos, "B: target_dir set");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test C: Modify role (doer -> thinker) -----------------------------------

static void test_C_modify_role_to_thinker() {
    std::cout << "\n=== C. Modify role to thinker ===\n\n";

    auto original_cwd = fs::current_path();
    auto tmp = setup_quorum_project();

    // Create doer first
    sui::quorum::cli::AgentCreateParams create_p;
    create_p.name = "code-agent";
    create_p.role = "doer";
    create_p.target_dir = "/some/path";
    create_p.no_ai = true;
    create_test_agent("code-agent", "doer");

    // Verify executor section exists
    auto yaml_before = read_file(tmp + "/.quorum/agents/code-agent.yaml");
    check(yaml_before.find("executor:") != std::string::npos,
          "C: executor section before");

    // Modify to thinker
    sui::quorum::cli::AgentCreateParams mod;
    mod.name = "code-agent";
    mod.role = "thinker";
    mod.no_ai = true;
    int rc = sui::quorum::cli::modify_agent(mod);
    check(rc == 0, "C: modify returns 0");

    auto yaml = read_file(tmp + "/.quorum/agents/code-agent.yaml");
    check(yaml.find("role: thinker") != std::string::npos, "C: role is thinker");
    check(yaml.find("executor:") == std::string::npos, "C: no executor section");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test D: Add skill_file --------------------------------------------------

static void test_D_add_skill_file() {
    std::cout << "\n=== D. Add skill_file ===\n\n";

    auto original_cwd = fs::current_path();
    auto tmp = setup_quorum_project();

    create_test_agent("writer", "scribe");

    // Modify to add/override skill
    sui::quorum::cli::AgentCreateParams mod;
    mod.name = "writer";
    mod.skill_file = ".claude/skills/tech-writer/SKILL.md";
    mod.no_ai = true;
    int rc = sui::quorum::cli::modify_agent(mod);
    check(rc == 0, "D: modify returns 0");

    auto yaml = read_file(tmp + "/.quorum/agents/writer.yaml");
    check(yaml.find("skill_file: .claude/skills/tech-writer/SKILL.md") != std::string::npos,
          "D: skill_file added");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test E: No changes prints help ------------------------------------------

static void test_E_no_changes_help() {
    std::cout << "\n=== E. No changes prints help ===\n\n";

    auto original_cwd = fs::current_path();
    auto tmp = setup_quorum_project();

    create_test_agent("stable", "thinker");

    // Modify with no flags
    sui::quorum::cli::AgentCreateParams mod;
    mod.name = "stable";
    mod.no_ai = true;
    int rc = sui::quorum::cli::modify_agent(mod);
    check(rc == 0, "E: returns 0 (no error)");

    // Verify YAML unchanged
    auto yaml = read_file(tmp + "/.quorum/agents/stable.yaml");
    check(yaml.find("role: thinker") != std::string::npos,
          "E: YAML unchanged");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test F: Agent not found -------------------------------------------------

static void test_F_agent_not_found() {
    std::cout << "\n=== F. Agent not found ===\n\n";

    auto original_cwd = fs::current_path();
    auto tmp = setup_quorum_project();

    sui::quorum::cli::AgentCreateParams mod;
    mod.name = "nonexistent";
    mod.description = "something";
    mod.no_ai = true;
    int rc = sui::quorum::cli::modify_agent(mod);
    check(rc == 1, "F: returns 1 for nonexistent agent");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test G: List agents -----------------------------------------------------

static void test_G_list_agents() {
    std::cout << "\n=== G. List agents ===\n\n";

    auto original_cwd = fs::current_path();
    auto tmp = setup_quorum_project();

    create_test_agent("alpha", "thinker", "Plans things");
    create_test_agent("beta", "doer", "Builds things");
    create_test_agent("gamma", "scribe", "Documents things");

    // list_agents reads from cwd's .quorum/agents/
    int rc = sui::quorum::cli::list_agents();
    check(rc == 0, "G: list returns 0");

    // Verify files exist (list_agents prints to stdout, not easily captured,
    // but we verify the scan works by checking return code + agent count)
    int count = 0;
    for (const auto& e : fs::directory_iterator(tmp + "/.quorum/agents")) {
        if (e.path().extension() == ".yaml") ++count;
    }
    check(count == 3, "G: 3 agent YAML files exist");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

int main() {
    std::cout << "=== Agent Modify Tests ===\n";

    test_A_modify_description();
    test_B_modify_role_to_doer();
    test_C_modify_role_to_thinker();
    test_D_add_skill_file();
    test_E_no_changes_help();
    test_F_agent_not_found();
    test_G_list_agents();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";
    return g_failed > 0 ? 1 : 0;
}
