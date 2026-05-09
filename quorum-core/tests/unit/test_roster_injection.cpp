// tests/unit/test_roster_injection.cpp
// Unit tests for team roster injection into agent prompts.
//
// Run:  cd build && cmake .. && make test_roster_injection && ./test_roster_injection

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

#include "utils/config.h"
#include "agent/context_assembler.h"

namespace fs = std::filesystem;

// --- helpers ----------------------------------------------------------------

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

// Create a temp directory with a CONTEXT.md file.
// Returns the directory path. Caller is responsible for cleanup.
static std::string make_temp_vault(const std::string& context_content) {
    auto tmp = fs::temp_directory_path() / ("test_roster_" + std::to_string(getpid()));
    fs::create_directories(tmp);
    std::ofstream f(tmp / "CONTEXT.md");
    f << context_content;
    f.close();
    return tmp.string();
}

static void cleanup_temp(const std::string& path) {
    fs::remove_all(path);
}

// --- Test A: Build roster with 3 agents -------------------------------------

static void test_build_roster_3_agents() {
    std::cout << "\n=== A. Build Roster with 3 Agents ===\n\n";

    std::vector<sui::quorum::AgentMetadata> agents;
    agents.push_back(sui::quorum::AgentMetadata{
        .id = "leader", .name = "Leader", .description = "coordinates", .role = "leader"
    });
    agents.push_back(sui::quorum::AgentMetadata{
        .id = "move-dev", .name = "Move Dev", .description = "writes Move", .role = "doer"
    });
    agents.push_back(sui::quorum::AgentMetadata{
        .id = "scribe", .name = "Scribe", .description = "records decisions", .role = "scribe"
    });

    sui::quorum::ConversationConfig cfg;
    cfg.leader = "leader";

    auto roster = sui::quorum::ContextAssembler::build_roster(agents, "move-dev", cfg);

    check(roster.find("**leader** (leader)") != std::string::npos,
          "A: contains '**leader** (leader)'");
    check(roster.find("**move-dev** (doer) <- you") != std::string::npos,
          "A: contains '**move-dev** (doer) <- you'");
    check(roster.find("**scribe** (scribe)") != std::string::npos,
          "A: contains '**scribe** (scribe)'");
    check(roster.find("coordinates") != std::string::npos,
          "A: contains 'coordinates' description");
    check(roster.find("writes Move") != std::string::npos,
          "A: contains 'writes Move' description");
    check(roster.find("records decisions") != std::string::npos,
          "A: contains 'records decisions' description");
}

// --- Test B: Build roster with default_path ---------------------------------

static void test_build_roster_default_path() {
    std::cout << "\n=== B. Build Roster with Default Path ===\n\n";

    std::vector<sui::quorum::AgentMetadata> agents;
    agents.push_back(sui::quorum::AgentMetadata{.id = "leader"});
    agents.push_back(sui::quorum::AgentMetadata{.id = "thinker"});
    agents.push_back(sui::quorum::AgentMetadata{.id = "doer"});

    sui::quorum::ConversationConfig cfg;
    cfg.default_path = {"leader", "thinker", "doer"};

    auto roster = sui::quorum::ContextAssembler::build_roster(agents, "leader", cfg);

    check(roster.find("leader -> thinker -> doer -> done") != std::string::npos,
          "B: contains 'leader -> thinker -> doer -> done'");
    check(roster.find("default path is configured") != std::string::npos,
          "B: contains 'default path is configured'");
}

// --- Test C: Build roster without default_path ------------------------------

static void test_build_roster_no_default_path() {
    std::cout << "\n=== C. Build Roster without Default Path ===\n\n";

    std::vector<sui::quorum::AgentMetadata> agents;
    agents.push_back(sui::quorum::AgentMetadata{.id = "leader"});

    sui::quorum::ConversationConfig cfg;
    // no default_path

    auto roster = sui::quorum::ContextAssembler::build_roster(agents, "leader", cfg);

    check(roster.find("You must include a HANDOFF block") != std::string::npos,
          "C: contains 'You must include a HANDOFF block'");
}

// --- Test D: Assemble with roster (team mode) -------------------------------

static void test_assemble_with_roster() {
    std::cout << "\n=== D. Assemble with Roster (Team Mode) ===\n\n";

    auto vault_dir = make_temp_vault("I am the test agent.");

    std::vector<sui::quorum::AgentMetadata> agents;
    agents.push_back(sui::quorum::AgentMetadata{
        .id = "test-agent", .name = "Test Agent", .role = "doer"
    });

    sui::quorum::ConversationConfig cfg;
    cfg.leader = "test-agent";

    auto roster = sui::quorum::ContextAssembler::build_roster(agents, "test-agent", cfg);

    sui::quorum::ContextAssembler assembler;
    auto output = assembler.assemble("test-agent", vault_dir, "turn", "Do the thing", roster);

    check(output.find("I am the test agent.") != std::string::npos,
          "D: output contains CONTEXT.md content");
    check(output.find("## Your Team") != std::string::npos,
          "D: output contains '## Your Team'");
    check(output.find("Do the thing") != std::string::npos,
          "D: output contains task description");
    check(output.find("HANDOFF") != std::string::npos,
          "D: output contains 'HANDOFF' (routing instructions)");
    // Phase 7 Track 5 inverted the pre-Track-5 contract: the output rules
    // block ("VAULT_UPDATE", "OBSERVATION", "# CRITICAL — Output Rules") is
    // stable identity and now lives unconditionally in system_prompt. The
    // legacy assemble() shim concatenates system_prompt + user_message, so
    // those tokens DO appear in team-mode output by design.
    check(output.find("VAULT_UPDATE") != std::string::npos,
          "D: output contains 'VAULT_UPDATE' (Track 5: rules always in system_prompt)");
    check(output.find("OBSERVATION") != std::string::npos,
          "D: output contains 'OBSERVATION' (Track 5: rules always in system_prompt)");

    cleanup_temp(vault_dir);
}

// --- Test E: Assemble without roster (legacy compat) ------------------------

static void test_assemble_without_roster() {
    std::cout << "\n=== E. Assemble without Roster (Legacy Compat) ===\n\n";

    auto vault_dir = make_temp_vault("I am the test agent.");

    sui::quorum::ContextAssembler assembler;
    auto output = assembler.assemble("test-agent", vault_dir, "turn", "Do the thing");

    check(output.find("VAULT_UPDATE") != std::string::npos,
          "E: output contains 'VAULT_UPDATE' (legacy rules present)");
    check(output.find("OBSERVATION") != std::string::npos,
          "E: output contains 'OBSERVATION' (legacy rules present)");
    check(output.find("## Your Team") == std::string::npos,
          "E: output does NOT contain '## Your Team'");

    cleanup_temp(vault_dir);
}

// --- Test G: Description and role fields parsed from YAML -------------------

static void test_yaml_description_role_parsing() {
    std::cout << "\n=== G. Description and Role Fields Parsed from YAML ===\n\n";

    auto tmp = fs::temp_directory_path() / ("test_agent_yaml_" + std::to_string(getpid()) + ".yaml");
    {
        std::ofstream f(tmp);
        f << "id: test-agent\n"
          << "name: \"Test Agent\"\n"
          << "role: doer\n"
          << "description: \"Writes and tests code\"\n"
          << "agent_class: executor\n"
          << "vault_path: data/vaults/test/\n"
          << "context_file: data/vaults/test/CONTEXT.md\n";
    }

    auto result = sui::quorum::load_agent_config(tmp.string());

    check(result.has_value(), "G: load_agent_config returned a value");
    check(result->id == "test-agent", "G: id == 'test-agent'");
    check(result->role == "doer", "G: role == 'doer'");
    check(result->description == "Writes and tests code", "G: description == 'Writes and tests code'");

    fs::remove(tmp);
}

// --- main -------------------------------------------------------------------

int main() {
    std::cout << "=== Roster Injection Tests ===\n";

    test_build_roster_3_agents();
    test_build_roster_default_path();
    test_build_roster_no_default_path();
    test_assemble_with_roster();
    test_assemble_without_roster();
    test_yaml_description_role_parsing();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
