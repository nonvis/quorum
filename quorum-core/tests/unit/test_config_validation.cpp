// tests/unit/test_config_validation.cpp
// Unit tests for config validation: auto-derive agent_class, skill_file parsing,
// validate_config() checks for leader, default_path, skill_file existence.
//
// Run:  cd build && cmake .. && make test_config_validation && ./test_config_validation

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

#include "utils/config.h"

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

static std::string make_temp_yaml(const std::string& content) {
    auto tmp = fs::temp_directory_path() /
        ("test_cfg_" + std::to_string(getpid()) + "_" + std::to_string(g_passed + g_failed) + ".yaml");
    std::ofstream f(tmp);
    f << content;
    f.close();
    return tmp.string();
}

static void cleanup_temp(const std::string& path) {
    fs::remove(path);
}

// --- Test A: Auto-derive agent_class from role: doer -> executor ------------

static void test_auto_derive_doer_to_executor() {
    std::cout << "\n=== A. Auto-derive agent_class from role: doer -> executor ===\n\n";

    auto path = make_temp_yaml("id: test\nrole: doer\n");
    auto result = sui::quorum::load_agent_config(path);

    check(result.has_value(), "A: load_agent_config returned a value");
    check(result->agent_class == "executor", "A: agent_class auto-derived to 'executor'");

    cleanup_temp(path);
}

// --- Test B: Explicit agent_class overrides auto-derive ---------------------

static void test_explicit_agent_class_overrides() {
    std::cout << "\n=== B. Explicit agent_class overrides auto-derive ===\n\n";

    auto path = make_temp_yaml("id: test\nrole: doer\nagent_class: analyst\n");
    auto result = sui::quorum::load_agent_config(path);

    check(result.has_value(), "B: load_agent_config returned a value");
    check(result->agent_class == "analyst", "B: explicit agent_class 'analyst' wins over auto-derive");

    cleanup_temp(path);
}

// --- Test C: Non-doer roles stay analyst ------------------------------------

static void test_non_doer_roles_stay_analyst() {
    std::cout << "\n=== C. Non-doer roles stay analyst ===\n\n";

    std::vector<std::string> roles = {"leader", "thinker", "evaluator"};
    for (const auto& role : roles) {
        auto path = make_temp_yaml("id: test\nrole: " + role + "\n");
        auto result = sui::quorum::load_agent_config(path);

        check(result.has_value(),
              ("C: load_agent_config returned a value for role '" + role + "'").c_str());
        check(result->agent_class == "analyst",
              ("C: role '" + role + "' stays agent_class 'analyst'").c_str());

        cleanup_temp(path);
    }
}

// --- Test D: skill_file parsed from YAML ------------------------------------

static void test_skill_file_parsed() {
    std::cout << "\n=== D. skill_file parsed from YAML ===\n\n";

    auto path = make_temp_yaml("id: test\nskill_file: path/to/skill.md\n");
    auto result = sui::quorum::load_agent_config(path);

    check(result.has_value(), "D: load_agent_config returned a value");
    check(result->skill_file == "path/to/skill.md", "D: skill_file == 'path/to/skill.md'");

    cleanup_temp(path);
}

// --- Test E: validate_config — leader not in agents -------------------------

static void test_validate_leader_not_found() {
    std::cout << "\n=== E. validate_config — leader not in agents ===\n\n";

    sui::quorum::QuorumConfig cfg;
    cfg.conversations.leader = "nonexistent";
    cfg.agents.push_back(sui::quorum::AgentMetadata{.id = "real"});

    bool valid = sui::quorum::validate_config(cfg);
    check(!valid, "E: validate_config returns false when leader not in agents");
}

// --- Test F: validate_config — default_path agent missing -------------------

static void test_validate_default_path_missing() {
    std::cout << "\n=== F. validate_config — default_path agent missing ===\n\n";

    sui::quorum::QuorumConfig cfg;
    cfg.conversations.default_path = {"leader", "ghost"};
    cfg.agents.push_back(sui::quorum::AgentMetadata{.id = "leader"});
    // "ghost" not in agents list

    bool valid = sui::quorum::validate_config(cfg);
    check(!valid, "F: validate_config returns false when default_path has missing agent");
}

// --- Test G: validate_config — everything valid -----------------------------

static void test_validate_all_valid() {
    std::cout << "\n=== G. validate_config — everything valid ===\n\n";

    sui::quorum::QuorumConfig cfg;
    cfg.conversations.leader = "leader";
    cfg.conversations.default_path = {"leader", "doer"};
    cfg.agents.push_back(sui::quorum::AgentMetadata{.id = "leader"});
    cfg.agents.push_back(sui::quorum::AgentMetadata{.id = "doer"});

    bool valid = sui::quorum::validate_config(cfg);
    check(valid, "G: validate_config returns true when everything is valid");
}

// --- main -------------------------------------------------------------------

int main() {
    std::cout << "=== Config Validation Tests ===\n";

    test_auto_derive_doer_to_executor();
    test_explicit_agent_class_overrides();
    test_non_doer_roles_stay_analyst();
    test_skill_file_parsed();
    test_validate_leader_not_found();
    test_validate_default_path_missing();
    test_validate_all_valid();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
