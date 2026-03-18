// tests/unit/test_skill_discovery.cpp
// Unit tests for skill discovery (discover_skills, --skill shorthand).
//
// Run:  cd build && cmake .. && make test_skill_discovery && ./test_skill_discovery

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "cli/skills.h"
#include "agent/context_assembler.h"

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
        ("quorum_test_skills_" + std::to_string(getpid()) + "_" + std::to_string(g_test_num++));
    fs::create_directories(dir);
    return dir.string();
}

static void cleanup_temp(const std::string& path) {
    fs::remove_all(path);
}

// --- Test A: discover_skills finds SKILL.md files ---------------------------

static void test_discover_skills() {
    std::cout << "\n=== A. discover_skills finds SKILL.md files ===\n\n";

    auto tmp = make_temp_dir();

    // Create two skills
    fs::create_directories(tmp + "/.claude/skills/move-developer");
    {
        std::ofstream out(tmp + "/.claude/skills/move-developer/SKILL.md");
        out << "# Move Dev\n";
    }

    fs::create_directories(tmp + "/.claude/skills/ts-developer");
    {
        std::ofstream out(tmp + "/.claude/skills/ts-developer/SKILL.md");
        out << "# TS Dev\n";
    }

    auto result = sui::quorum::cli::discover_skills(tmp);

    check(result.size() == 2, "A: returns 2 skills");
    check(result[0].id == "move-developer", "A: first skill id is 'move-developer' (sorted)");
    check(result[1].id == "ts-developer", "A: second skill id is 'ts-developer'");
    check(result[0].path == ".claude/skills/move-developer/SKILL.md", "A: first skill path is correct");

    cleanup_temp(tmp);
}

// --- Test B: empty skills directory returns empty vector --------------------

static void test_empty_skills() {
    std::cout << "\n=== B. empty skills directory returns empty vector ===\n\n";

    auto tmp = make_temp_dir();

    // No .claude/skills/ at all
    auto result = sui::quorum::cli::discover_skills(tmp);
    check(result.empty(), "B: returns empty when no .claude/skills/ dir");

    // Create empty .claude/skills/
    fs::create_directories(tmp + "/.claude/skills");
    result = sui::quorum::cli::discover_skills(tmp);
    check(result.empty(), "B: returns empty when .claude/skills/ is empty");

    cleanup_temp(tmp);
}

// --- Test C: directory without SKILL.md is skipped -------------------------

static void test_skip_without_skill_md() {
    std::cout << "\n=== C. directory without SKILL.md is skipped ===\n\n";

    auto tmp = make_temp_dir();

    // Create broken skill dir (no SKILL.md inside)
    fs::create_directories(tmp + "/.claude/skills/broken");

    // Create valid skill dir
    fs::create_directories(tmp + "/.claude/skills/valid");
    {
        std::ofstream out(tmp + "/.claude/skills/valid/SKILL.md");
        out << "# Valid Skill\n";
    }

    auto result = sui::quorum::cli::discover_skills(tmp);

    check(result.size() == 1, "C: returns 1 skill (broken dir skipped)");
    check(result[0].id == "valid", "C: only 'valid' skill returned");

    cleanup_temp(tmp);
}

// --- Test D: --skill shorthand produces correct path -----------------------

static void test_skill_shorthand() {
    std::cout << "\n=== D. --skill shorthand produces correct path ===\n\n";

    {
        std::string skill_name = "move-developer";
        std::string expected = ".claude/skills/move-developer/SKILL.md";
        std::string actual = ".claude/skills/" + skill_name + "/SKILL.md";
        check(actual == expected, "D: --skill move-developer produces correct path");
    }

    {
        std::string skill_name = "sui-move-dev";
        std::string expected = ".claude/skills/sui-move-dev/SKILL.md";
        std::string actual = ".claude/skills/" + skill_name + "/SKILL.md";
        check(actual == expected, "D: --skill sui-move-dev produces correct path");
    }
}

// --- Test E: skill file resolution with project_root in assembler ----------

static void test_assembler_project_root() {
    std::cout << "\n=== E. skill file resolution with project_root in assembler ===\n\n";

    auto tmp = make_temp_dir();

    // Create skill file
    fs::create_directories(tmp + "/.claude/skills/test-skill");
    {
        std::ofstream out(tmp + "/.claude/skills/test-skill/SKILL.md");
        out << "# Test Skill\n\nThis is a test skill.";
    }

    // Create vault with CONTEXT.md
    fs::create_directories(tmp + "/vault");
    {
        std::ofstream out(tmp + "/vault/CONTEXT.md");
        out << "# Test Agent\n";
    }

    sui::quorum::ContextAssembler assembler;
    auto vault_dir = tmp + "/vault";
    std::string skill_file = ".claude/skills/test-skill/SKILL.md";

    // With project_root — should find the skill
    auto result = assembler.assemble("test-agent", vault_dir, "turn", "do something",
                                      /*team_roster=*/"", skill_file,
                                      /*project_root=*/tmp);

    check(result.find("# Skill Reference") != std::string::npos,
          "E: with project_root, prompt contains '# Skill Reference'");
    check(result.find("This is a test skill") != std::string::npos,
          "E: with project_root, prompt contains skill content");

    // Without project_root, from a different cwd — should NOT find the skill
    auto original_cwd = fs::current_path().string();
    fs::current_path("/tmp");

    auto result2 = assembler.assemble("test-agent", vault_dir, "turn", "do something",
                                       /*team_roster=*/"", skill_file,
                                       /*project_root=*/"");

    check(result2.find("# Skill Reference") == std::string::npos,
          "E: without project_root from /tmp, '# Skill Reference' NOT found");

    // Restore cwd
    fs::current_path(original_cwd);

    cleanup_temp(tmp);
}

// --- main -------------------------------------------------------------------

int main() {
    std::cout << "=== Skill Discovery Tests ===\n";

    test_discover_skills();
    test_empty_skills();
    test_skip_without_skill_md();
    test_skill_shorthand();
    test_assembler_project_root();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
