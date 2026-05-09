// tests/unit/test_assembler_split.cpp
// Phase 7 Track 5 — assemble_split() returns {system_prompt, user_message}.
//
// system_prompt is the stable identity prefix (CONTEXT.md + SKILL.md +
// output rules) that gets passed to `claude -p` via
// --append-system-prompt-file. user_message holds rules, refs, inbox, roster,
// and the per-task body, piped to the claude process via stdin. The split
// lets Anthropic's prefix-cache amortize the stable section across turns.
//
// Run:  cd build && ctest -R test_assembler_split --output-on-failure

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <unistd.h>

#include "agent/context_assembler.h"

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

static std::string make_temp_vault() {
    auto dir = fs::temp_directory_path() /
        ("quorum_test_split_" + std::to_string(getpid()) + "_" +
         std::to_string(g_test_num++));
    fs::create_directories(dir);
    fs::create_directories(dir / "knowledge");
    fs::create_directories(dir / "inbox");
    return dir.string();
}

static void cleanup(const std::string& path) {
    fs::remove_all(path);
}

static void write_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path);
    out << content;
}

static void write_knowledge(const std::string& vault, const std::string& name,
                            const std::string& content) {
    write_file(fs::path(vault) / "knowledge" / name, content);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
}

// --- S1: CONTEXT.md goes to system_prompt only ------------------------------

static void test_s1_context_in_system_prompt() {
    std::cout << "\n=== S1. CONTEXT.md in system_prompt, NOT user_message ===\n\n";

    auto vault = make_temp_vault();
    write_file(fs::path(vault) / "CONTEXT.md", "AGENT_CTX_TOKEN_S1\n");

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "agent-s1", vault, "turn", "task body S1");

    check(split.system_prompt.find("AGENT_CTX_TOKEN_S1\n") != std::string::npos,
          "S1: CONTEXT.md content present in system_prompt");
    check(split.user_message.find("AGENT_CTX_TOKEN_S1\n") == std::string::npos,
          "S1: CONTEXT.md content absent from user_message");
    check(split.system_prompt.find("# Agent Context") != std::string::npos,
          "S1: '# Agent Context' header present in system_prompt");

    cleanup(vault);
}

// --- S2: SKILL.md goes to system_prompt only --------------------------------

static void test_s2_skill_in_system_prompt() {
    std::cout << "\n=== S2. SKILL.md in system_prompt, NOT user_message ===\n\n";

    auto vault = make_temp_vault();
    auto skill_path = fs::path(vault) / "SKILL.md";
    write_file(skill_path, "SKILL_TOKEN_S2\n");

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "agent-s2", vault, "turn", "task body S2",
        /*team_roster=*/{}, /*skill_file=*/skill_path.string());

    check(split.system_prompt.find("SKILL_TOKEN_S2\n") != std::string::npos,
          "S2: SKILL.md content present in system_prompt");
    check(split.user_message.find("SKILL_TOKEN_S2\n") == std::string::npos,
          "S2: SKILL.md content absent from user_message");
    check(split.system_prompt.find("# Skill Reference") != std::string::npos,
          "S2: '# Skill Reference' header present in system_prompt");

    cleanup(vault);
}

// --- S3: Output Rules / Output Instructions block in system_prompt ----------

static void test_s3_output_rules_in_system_prompt() {
    std::cout << "\n=== S3. Output Rules + Output Instructions in system_prompt ===\n\n";

    auto vault = make_temp_vault();

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "agent-s3", vault, "turn", "task body S3");

    check(split.system_prompt.find("# CRITICAL — Output Rules") != std::string::npos,
          "S3: '# CRITICAL — Output Rules' header in system_prompt");
    check(split.system_prompt.find("# Output Instructions") != std::string::npos,
          "S3: '# Output Instructions' header in system_prompt");
    check(split.user_message.find("# CRITICAL — Output Rules") == std::string::npos,
          "S3: Output Rules NOT in user_message");
    check(split.user_message.find("# Output Instructions") == std::string::npos,
          "S3: Output Instructions NOT in user_message");

    // S3b: Output Rules emitted in system_prompt EVEN in team mode
    // (regardless of team_roster value). Stable identity is identity.
    auto split_team = assembler.assemble_split(
        "agent-s3", vault, "turn", "task body S3 team",
        /*team_roster=*/"## Your Team\n- agent-s3 <- you\n");
    check(split_team.system_prompt.find("# CRITICAL — Output Rules") != std::string::npos,
          "S3: Output Rules also in system_prompt under team mode");

    cleanup(vault);
}

// --- S4: Task description in user_message, NOT in system_prompt ------------

static void test_s4_task_in_user_message() {
    std::cout << "\n=== S4. Task description in user_message only ===\n\n";

    auto vault = make_temp_vault();

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "agent-s4", vault, "turn", "TASK_BODY_TOKEN_S4");

    check(split.user_message.find("TASK_BODY_TOKEN_S4") != std::string::npos,
          "S4: task description present in user_message");
    check(split.system_prompt.find("TASK_BODY_TOKEN_S4") == std::string::npos,
          "S4: task description NOT in system_prompt");
    check(split.user_message.find("# Current Task") != std::string::npos,
          "S4: '# Current Task' header in user_message");

    cleanup(vault);
}

// --- S5: same agent vault + different tasks → identical system_prompt -------
//
// This is the core invariant that justifies the split: the prefix Anthropic
// caches must be byte-identical across consecutive turns for the same agent.

static void test_s5_system_prompt_stable_across_tasks() {
    std::cout << "\n=== S5. system_prompt byte-equal across two task bodies ===\n\n";

    auto vault = make_temp_vault();
    write_file(fs::path(vault) / "CONTEXT.md", "STABLE_CTX_S5\n");
    auto skill_path = fs::path(vault) / "SKILL.md";
    write_file(skill_path, "STABLE_SKILL_S5\n");

    sui::quorum::ContextAssembler assembler;

    auto split1 = assembler.assemble_split(
        "agent-s5", vault, "turn", "task body ALPHA",
        {}, skill_path.string());
    auto split2 = assembler.assemble_split(
        "agent-s5", vault, "turn", "task body BETA — totally different content",
        {}, skill_path.string());

    check(split1.system_prompt == split2.system_prompt,
          "S5: system_prompt is byte-identical across two distinct task bodies");
    check(split1.user_message != split2.user_message,
          "S5: user_message differs between the two task bodies");

    cleanup(vault);
}

// --- S6: rules + roster appear in user_message (variable per task) ---------

static void test_s6_rules_and_roster_in_user_message() {
    std::cout << "\n=== S6. rules + roster appear in user_message ===\n\n";

    auto vault = make_temp_vault();
    write_knowledge(vault, "rule-foo.md", "RULE_BODY_S6_FOO\n");
    auto roster = "## Your Team\n- agent-s6 <- you\n- other (doer)\n";

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "agent-s6", vault, "turn", "task S6",
        /*team_roster=*/roster);

    check(split.user_message.find("RULE_BODY_S6_FOO\n") != std::string::npos,
          "S6: rule body in user_message");
    check(split.system_prompt.find("RULE_BODY_S6_FOO\n") == std::string::npos,
          "S6: rule body NOT in system_prompt");
    check(split.user_message.find("# Knowledge: rule-foo.md") != std::string::npos,
          "S6: '# Knowledge: rule-foo.md' header in user_message");
    check(split.user_message.find("## Your Team") != std::string::npos,
          "S6: roster heading in user_message");
    check(split.system_prompt.find("## Your Team") == std::string::npos,
          "S6: roster NOT in system_prompt");

    cleanup(vault);
}

// --- main -------------------------------------------------------------------

int main() {
    std::cout << "=====================================================\n";
    std::cout << "  Phase 7 Track 5 — assembler split tests\n";
    std::cout << "=====================================================\n";

    test_s1_context_in_system_prompt();
    test_s2_skill_in_system_prompt();
    test_s3_output_rules_in_system_prompt();
    test_s4_task_in_user_message();
    test_s5_system_prompt_stable_across_tasks();
    test_s6_rules_and_roster_in_user_message();

    std::cout << "\n---------------------------------------------------\n";
    std::cout << "  passed: " << g_passed << "  failed: " << g_failed << "\n";
    std::cout << "---------------------------------------------------\n";

    return g_failed == 0 ? 0 : 1;
}
