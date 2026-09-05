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
    p.role = "thinker";
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

// --- Test F2: --skill <name> resolves to $HOME path when not project-local --
// Phase 9 finding #3 — main.cpp parses `--skill <name>` into
// `.claude/skills/<name>/SKILL.md`. When that doesn't exist project-locally,
// create_agent should fall back to $HOME/.claude/skills/<name>/SKILL.md.

static void test_skill_home_fallback() {
    std::cout << "\n=== F2. --skill <name> falls back to $HOME (#3) ===\n\n";

    auto tmp = make_temp_dir();
    auto data_dir = tmp + "/data";

    // Create a fake $HOME with a skill file at the expected layout.
    auto fake_home = tmp + "/fakehome";
    auto fake_skill_dir = fake_home + "/.claude/skills/fake-role";
    fs::create_directories(fake_skill_dir);
    {
        std::ofstream f(fake_skill_dir + "/SKILL.md");
        f << "---\nname: fake-role\n---\n# Fake role skill\n";
    }

    // Save + override HOME for the duration of this test.
    const char* prev_home = std::getenv("HOME");
    std::string saved_home = prev_home ? prev_home : "";
    setenv("HOME", fake_home.c_str(), 1);

    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    sui::quorum::cli::AgentCreateParams p;
    p.role = "thinker";
    p.name = "home-skill-agent";
    p.project = "test-proj";
    p.data_dir = data_dir;
    // Mimic what main.cpp's `--skill fake-role` parsing produces: a
    // project-relative path that doesn't exist project-locally.
    p.skill_file = ".claude/skills/fake-role/SKILL.md";
    p.no_ai = true;

    int rc = sui::quorum::cli::create_agent(p);
    check(rc == 0, "F2: create_agent returns 0");

    auto yaml = read_file("configs/agents/test-proj/home-skill-agent.yaml");
    auto expected = std::string("skill_file: ") + fake_home + "/.claude/skills/fake-role/SKILL.md";
    check(yaml.find(expected) != std::string::npos,
          "F2: YAML stores $HOME-expanded skill path when project-local missing");

    // Restore HOME.
    if (prev_home) setenv("HOME", saved_home.c_str(), 1);
    else unsetenv("HOME");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test G: Evaluator role end-to-end (Phase 8 Track 6 #25) ----------------

static void test_evaluator_role_create() {
    std::cout << "\n=== G. Evaluator role end-to-end ===\n\n";

    auto tmp = make_temp_dir();
    auto data_dir = tmp + "/data";

    auto original_cwd = fs::current_path();
    fs::current_path(tmp);

    sui::quorum::cli::AgentCreateParams p;
    p.role = "evaluator";
    p.name = "eval";
    p.project = "test-proj";
    p.data_dir = data_dir;
    p.no_ai = true;

    int rc = sui::quorum::cli::create_agent(p);

    check(rc == 0, "G: create_agent returns 0 for evaluator role");
    check(fs::exists("configs/agents/test-proj/eval.yaml"),
          "G: eval.yaml is created");
    check(fs::exists(data_dir + "/vaults/eval"),
          "G: vault directory is created");
    check(fs::exists(data_dir + "/vaults/eval/CONTEXT.md"),
          "G: CONTEXT.md is generated");

    auto yaml = read_file("configs/agents/test-proj/eval.yaml");
    check(yaml.find("role: evaluator") != std::string::npos,
          "G: YAML contains 'role: evaluator'");
    check(yaml.find("executor:") == std::string::npos,
          "G: YAML does NOT contain 'executor:' (evaluator is analyst-class)");

    fs::current_path(original_cwd);
    cleanup_temp(tmp);
}

// --- Test H: universal_rules_for_role("evaluator") returns correct rules ----

static void test_evaluator_universal_rules() {
    std::cout << "\n=== H. universal_rules_for_role(evaluator) ===\n\n";

    auto rules = sui::quorum::cli::universal_rules_for_role("evaluator");

    check(rules.find("HANDOFF to done") != std::string::npos,
          "H: rules contain 'HANDOFF to done' (Phase 14: no scribe)");
    check(rules.find("Do NOT modify") != std::string::npos,
          "H: rules contain 'Do NOT modify'");
    check(rules.find("Preserve and use the task number") != std::string::npos,
          "H: rules contain 'Preserve and use the task number'");
    // Universal rule #9 about self-contained HANDOFF prompts is present
    check(rules.find("self-contained") != std::string::npos,
          "H: rules contain 'self-contained' (universal rule #9)");
}

// --- Test I: evaluator SKILL.md exists at the expected source path ----------

static void test_evaluator_skill_source_exists() {
    std::cout << "\n=== I. evaluator/SKILL.md exists in templates ===\n\n";

    static constexpr const char* kRel =
        "/templates/skills/quorum-roles/evaluator/SKILL.md";

    std::vector<std::string> candidates;
#ifdef QUORUM_SOURCE_ROOT
    // First choice: the repo root baked in at compile time by CMakeLists.txt.
    // Independent of cwd, so the test is green from ANY build directory —
    // the cwd ladder below is only correct when ctest runs inside the repo.
    candidates.emplace_back(std::string(QUORUM_SOURCE_ROOT) + kRel);
#endif
    // Fallback ladder, kept for a hand-compiled binary run without the define.
    candidates.emplace_back(std::string(".") + kRel);
    candidates.emplace_back(std::string("..") + kRel);
    candidates.emplace_back(std::string("../..") + kRel);

    bool found = false;
    std::string found_at;
    for (const auto& c : candidates) {
        if (fs::exists(c)) {
            found = true;
            found_at = c;
            break;
        }
    }

    check(found, "I: evaluator/SKILL.md found in templates tree");
    if (found) {
        // Sanity-check key content: lint-required headers + EVALUATION block
        std::ifstream f(found_at);
        std::string content{
            std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>()
        };
        check(content.find("Block Formats") != std::string::npos,
              "I: SKILL.md contains 'Block Formats' (lint-required)");
        check(content.find("HANDOFF") != std::string::npos,
              "I: SKILL.md contains 'HANDOFF' (lint-required)");
        check(content.find("SUMMARY") != std::string::npos,
              "I: SKILL.md contains 'SUMMARY' (lint-required)");
        check(content.find("EVALUATION") != std::string::npos,
              "I: SKILL.md defines an EVALUATION block");
    }
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
    test_skill_home_fallback();
    test_evaluator_role_create();
    test_evaluator_universal_rules();
    test_evaluator_skill_source_exists();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
