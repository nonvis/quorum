// tests/unit/test_agent_roster_dir.cpp
// Unit tests for directory-based agent loading (load_agents_from_directory).
//
// Run:  cd build && cmake .. && make test_agent_roster_dir && ./test_agent_roster_dir

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "utils/config.h"

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
        ("quorum_test_roster_" + std::to_string(getpid()) + "_" + std::to_string(g_test_num++));
    fs::create_directories(dir);
    return dir.string();
}

static void cleanup_temp(const std::string& path) {
    fs::remove_all(path);
}

// --- Test A: load agents from directory --------------------------------------

static void test_load_agents_from_directory() {
    std::cout << "\n=== A. load agents from directory ===\n\n";

    auto tmp = make_temp_dir();

    // Create 3 valid agent YAMLs
    {
        std::ofstream out(tmp + "/alpha.yaml");
        out << "id: alpha\nname: Alpha\nrole: thinker\n";
    }
    {
        std::ofstream out(tmp + "/beta.yaml");
        out << "id: beta\nname: Beta\nrole: doer\n";
    }
    {
        std::ofstream out(tmp + "/gamma.yaml");
        out << "id: gamma\nname: Gamma\nrole: scribe\n";
    }

    auto agents = sui::quorum::load_agents_from_directory(tmp);

    check(agents.size() == 3, "A: returns 3 agents");

    cleanup_temp(tmp);
}

// --- Test B: alphabetical ordering -------------------------------------------

static void test_alphabetical_ordering() {
    std::cout << "\n=== B. alphabetical ordering ===\n\n";

    auto tmp = make_temp_dir();

    {
        std::ofstream out(tmp + "/z-agent.yaml");
        out << "id: z-agent\nname: Z Agent\nrole: thinker\n";
    }
    {
        std::ofstream out(tmp + "/a-agent.yaml");
        out << "id: a-agent\nname: A Agent\nrole: leader\n";
    }
    {
        std::ofstream out(tmp + "/m-agent.yaml");
        out << "id: m-agent\nname: M Agent\nrole: doer\n";
    }

    auto agents = sui::quorum::load_agents_from_directory(tmp);

    check(agents.size() == 3, "B: returns 3 agents");
    check(agents[0].id == "a-agent", "B: first is 'a-agent'");
    check(agents[1].id == "m-agent", "B: second is 'm-agent'");
    check(agents[2].id == "z-agent", "B: third is 'z-agent'");

    cleanup_temp(tmp);
}

// --- Test C: invalid YAML skipped --------------------------------------------

static void test_invalid_yaml_skipped() {
    std::cout << "\n=== C. invalid YAML skipped ===\n\n";

    auto tmp = make_temp_dir();

    // Valid agent
    {
        std::ofstream out(tmp + "/good.yaml");
        out << "id: good\nname: Good Agent\nrole: thinker\n";
    }
    // Invalid agent (no id field)
    {
        std::ofstream out(tmp + "/bad.yaml");
        out << "role: doer\nname: Bad Agent\n";
    }

    auto agents = sui::quorum::load_agents_from_directory(tmp);

    check(agents.size() == 1, "C: returns 1 agent (invalid skipped)");
    check(agents[0].id == "good", "C: valid agent loaded");

    cleanup_temp(tmp);
}

// --- Test D: empty directory -------------------------------------------------

static void test_empty_directory() {
    std::cout << "\n=== D. empty directory ===\n\n";

    auto tmp = make_temp_dir();

    auto agents = sui::quorum::load_agents_from_directory(tmp);

    check(agents.empty(), "D: returns empty vector for empty directory");

    cleanup_temp(tmp);
}

// --- Test E: non-YAML files ignored ------------------------------------------

static void test_non_yaml_files_ignored() {
    std::cout << "\n=== E. non-YAML files ignored ===\n\n";

    auto tmp = make_temp_dir();

    // Non-YAML files
    {
        std::ofstream out(tmp + "/readme.txt");
        out << "id: txt-agent\nrole: thinker\n";
    }
    {
        std::ofstream out(tmp + "/notes.md");
        out << "id: md-agent\nrole: leader\n";
    }
    // One valid YAML
    {
        std::ofstream out(tmp + "/real.yaml");
        out << "id: real\nname: Real Agent\nrole: doer\n";
    }

    auto agents = sui::quorum::load_agents_from_directory(tmp);

    check(agents.size() == 1, "E: only 1 agent loaded (non-YAML ignored)");
    check(agents[0].id == "real", "E: loaded agent is 'real'");

    cleanup_temp(tmp);
}

// --- Test F: directory scan overrides config agents list ---------------------

static void test_directory_scan_overrides_config() {
    std::cout << "\n=== F. directory scan overrides config agents list ===\n\n";

    auto tmp = make_temp_dir();
    auto agents_dir = tmp + "/agents";
    fs::create_directories(agents_dir);

    // Write config.yaml with explicit agents: section
    {
        std::ofstream out(tmp + "/config.yaml");
        out << "conversations:\n"
            << "  enabled: true\n"
            << "  leader: config-agent\n"
            << "\n"
            << "agents:\n"
            << "  - config: " << agents_dir << "/config-agent.yaml\n";
    }

    // Create agent that IS listed in config.yaml
    {
        std::ofstream out(agents_dir + "/config-agent.yaml");
        out << "id: config-agent\nname: Config Agent\nrole: leader\n";
    }

    // Create agent that is NOT listed in config.yaml but lives in agents/
    {
        std::ofstream out(agents_dir + "/dir-agent.yaml");
        out << "id: dir-agent\nname: Dir Agent\nrole: doer\n";
    }

    auto cfg = sui::quorum::load_config(tmp + "/config.yaml");

    check(cfg.has_value(), "F: load_config succeeds");
    check(cfg->agents.size() == 2, "F: 2 agents loaded from directory scan");

    // Sorted alphabetically: config-agent, dir-agent
    check(cfg->agents[0].id == "config-agent", "F: first agent is 'config-agent'");
    check(cfg->agents[1].id == "dir-agent", "F: second agent is 'dir-agent' (from directory, not in config list)");

    cleanup_temp(tmp);
}

// --- Test G: fallback to config list when no agents/ directory ---------------

static void test_fallback_to_config_list() {
    std::cout << "\n=== G. fallback to config list when no agents/ directory ===\n\n";

    auto tmp = make_temp_dir();

    // Write the agent YAML at top level (NOT in an agents/ subdir)
    {
        std::ofstream out(tmp + "/the-agent.yaml");
        out << "id: fallback-agent\nname: Fallback Agent\nrole: leader\n";
    }

    // Write config.yaml pointing to the agent with absolute path
    {
        std::ofstream out(tmp + "/config.yaml");
        out << "conversations:\n"
            << "  enabled: true\n"
            << "  leader: fallback-agent\n"
            << "\n"
            << "agents:\n"
            << "  - config: " << tmp << "/the-agent.yaml\n";
    }

    // No agents/ subdirectory exists
    auto cfg = sui::quorum::load_config(tmp + "/config.yaml");

    check(cfg.has_value(), "G: load_config succeeds");
    check(cfg->agents.size() == 1, "G: 1 agent loaded from explicit config list");
    check(cfg->agents[0].id == "fallback-agent", "G: agent is 'fallback-agent' (from config list fallback)");

    cleanup_temp(tmp);
}

// --- main -------------------------------------------------------------------

int main() {
    std::cout << "=== Agent Roster Directory Tests ===\n";

    test_load_agents_from_directory();
    test_alphabetical_ordering();
    test_invalid_yaml_skipped();
    test_empty_directory();
    test_non_yaml_files_ignored();
    test_directory_scan_overrides_config();
    test_fallback_to_config_list();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
