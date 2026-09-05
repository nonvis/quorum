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

// --- S7: the VERDICT rule rides in the stable Output Rules block ------------
//
// Tier 1 of the daemon's task summary (output_parser.h extract_summary) prefers
// an explicit `VERDICT:` line. The four quorum-roles SKILLs carry the same
// bullet, but a SKILL is not a reliable carrier: a `quorum init` leader has no
// skill_file at all, and every knower loads its DOMAIN skill instead of the
// role skill (the assembler loads exactly one skill_file). The Output Rules
// block is the one block EVERY agent prompt gets, so the rule lives there.
//
// NOTE ON SCOPE: the assembler has exactly ONE Output Rules emitter — it is
// unconditional, so the analyst / executor / brainstorm shapes below all run
// the same code. They are regression guards against the block being made
// conditional again: it WAS suppressed for team mode before Phase 7 Track 5
// (see the comment at the emitter). A single mutation therefore reds S7a-S7c
// together; S7d (placement) is independent.

static const char* const kVerdictLead = "**End with a one-line verdict.**";
static const char* const kVerdictForm =
    "`VERDICT: <one sentence — what you did or decided, ≤ 25 words>`";

static void test_s7_verdict_rule_in_output_rules() {
    std::cout << "\n=== S7. VERDICT rule in the stable Output Rules block ===\n\n";

    auto vault = make_temp_vault();
    sui::quorum::ContextAssembler assembler;

    // S7a: analyst-class agent (role `thinker`).
    auto analyst = assembler.assemble_split(
        "agent-s7-analyst", vault, "turn", "task body S7 analyst",
        /*team_roster=*/{}, /*skill_file=*/{}, /*project_root=*/{},
        /*agent_role=*/"thinker");
    check(analyst.system_prompt.find(kVerdictLead) != std::string::npos,
          "S7a: analyst system_prompt carries the VERDICT rule");
    check(analyst.system_prompt.find(kVerdictForm) != std::string::npos,
          "S7a: analyst system_prompt carries the exact VERDICT line form");

    // S7b: executor-class agent (role `doer`).
    auto executor = assembler.assemble_split(
        "agent-s7-doer", vault, "turn", "task body S7 doer",
        /*team_roster=*/{}, /*skill_file=*/{}, /*project_root=*/{},
        /*agent_role=*/"doer");
    check(executor.system_prompt.find(kVerdictLead) != std::string::npos,
          "S7b: executor system_prompt carries the VERDICT rule");

    // S7c: brainstorm mode, with a roster (the team-mode suppression hazard).
    auto brainstorm = assembler.assemble_split(
        "agent-s7-brain", vault, "turn", "task body S7 brainstorm",
        /*team_roster=*/"## Your Team\n- agent-s7-brain <- you\n",
        /*skill_file=*/{}, /*project_root=*/{}, /*agent_role=*/"thinker",
        /*budget=*/{}, /*conversation_mode=*/"brainstorm");
    check(brainstorm.system_prompt.find(kVerdictLead) != std::string::npos,
          "S7c: brainstorm-mode system_prompt carries the VERDICT rule");

    // S7d: placement — stable half only. The per-task user_message must not
    // mention VERDICT at all, or the cached prefix stops being the whole rule
    // set and every turn re-sends it.
    check(analyst.user_message.find("VERDICT") == std::string::npos,
          "S7d: VERDICT absent from user_message (prefix-cache hygiene)");

    // S7e: the legacy single-string assemble() shim carries it too — that is
    // the path test_assembler_rule_cap and the pipeline test still use.
    auto legacy = assembler.assemble(
        "agent-s7-legacy", vault, "turn", "task body S7 legacy");
    check(legacy.find(kVerdictLead) != std::string::npos,
          "S7e: legacy assemble() output carries the VERDICT rule");

    cleanup(vault);
}

// --- S8: the Output Rules block is CLASS-AWARE -------------------------------
//
// Before Phase 15 one analyst-only block went to every agent, so a doer
// (executor class, full tool grant, cwd = target_dir) was ordered "NEVER write
// files directly" and "NEVER run commands that modify files" — contradicting
// both its tool surface and its own quorum-roles/doer SKILL.
//
// Class is derived by ContextAssembler::effective_agent_class(), a mirror of
// the canonical rule in utils/config.h load_agent_config(): explicit
// `agent_class:` wins, else role "doer" => executor. Brainstorm rides over
// class the same way invoker.h build_tool_flags() does.

// Everything from the Output Rules header up to the next block header.
static std::string extract_output_rules(const std::string& sp) {
    auto b = sp.find("# CRITICAL — Output Rules");
    if (b == std::string::npos) return {};
    auto e = sp.find("# Output Instructions", b);
    return e == std::string::npos ? sp.substr(b) : sp.substr(b, e - b);
}

// The analyst block, pinned byte for byte. Any edit to the analyst text — even
// a single character — must come here deliberately.
static const char* const kAnalystRulesGolden =
    "# CRITICAL — Output Rules\n"
    "\n"
    "You MUST follow these rules for ALL output:\n"
    "\n"
    "1. **NEVER write files directly.** Do not use Write, Edit, or any file-creation tool. "
    "All output goes in your response text as structured blocks.\n"
    "2. **NEVER run commands that modify files.** You may READ files and RUN queries "
    "(sqlite3, cat, ls, grep), but never write, move, or delete.\n"
    "3. **ALL findings must use structured blocks** in your response: "
    "VAULT_UPDATE, OBSERVATION, PROPOSAL, SUMMARY.\n"
    "4. **Only write to YOUR vault.** VAULT_UPDATE paths must start with `knowledge/` or `inbox/`.\n"
    "5. **End with a one-line verdict.** The last line of your reply before any HANDOFF block is "
    "`VERDICT: <one sentence — what you did or decided, ≤ 25 words>`. "
    "The daemon stores it as the task's summary. Never leave it blank.\n"
    "\n"
    "The daemon extracts these blocks from your response text and routes them. "
    "If you write files directly, the daemon cannot track your output.\n"
    "\n"
    "---\n"
    "\n";

static const char* const kAnalystRule1 = "**NEVER write files directly.**";
static const char* const kExecTargetDir = "**Write ONLY inside your `target_dir`.**";

static void test_s8_output_rules_are_class_aware() {
    std::cout << "\n=== S8. Output Rules block is class-aware ===\n\n";

    auto vault = make_temp_vault();
    sui::quorum::ContextAssembler assembler;

    // --- the derivation itself (pure), mirroring utils/config.h ------------
    using CA = sui::quorum::ContextAssembler;
    check(CA::effective_agent_class("doer", "", "") == "executor",
          "S8a: role 'doer' derives executor");
    check(CA::effective_agent_class("thinker", "", "") == "analyst",
          "S8a: role 'thinker' derives analyst");
    check(CA::effective_agent_class("leader", "", "") == "analyst",
          "S8a: role 'leader' derives analyst");
    check(CA::effective_agent_class("", "", "") == "analyst",
          "S8a: unknown/empty role derives analyst (safe default)");
    check(CA::effective_agent_class("doer", "analyst", "") == "analyst",
          "S8a: an explicit agent_class overrides the role");
    check(CA::effective_agent_class("doer", "executor", "brainstorm") == "analyst",
          "S8a: brainstorm rides over BOTH role and explicit class");

    // --- analyst block, byte-pinned ----------------------------------------
    auto analyst = assembler.assemble_split(
        "agent-s8-analyst", vault, "turn", "task S8 analyst",
        /*team_roster=*/{}, /*skill_file=*/{}, /*project_root=*/{},
        /*agent_role=*/"thinker");
    auto analyst_block = extract_output_rules(analyst.system_prompt);
    if (analyst_block != kAnalystRulesGolden) {
        std::cerr << "--- GOLDEN (expected) ---\n" << kAnalystRulesGolden
                  << "--- ACTUAL ---\n" << analyst_block << "--- END ---\n";
    }
    check(analyst_block == kAnalystRulesGolden,
          "S8b: analyst Output Rules block is byte-identical to the golden");

    // --- executor block -----------------------------------------------------
    auto executor = assembler.assemble_split(
        "agent-s8-doer", vault, "turn", "task S8 doer",
        /*team_roster=*/{}, /*skill_file=*/{}, /*project_root=*/{},
        /*agent_role=*/"doer");
    check(executor.system_prompt.find(kExecTargetDir) != std::string::npos,
          "S8c: executor prompt carries the target_dir write-scope rule");
    check(executor.system_prompt.find(kAnalystRule1) == std::string::npos,
          "S8c: executor prompt does NOT say 'NEVER write files directly'");
    check(executor.system_prompt.find("NEVER run commands that modify files")
              == std::string::npos,
          "S8c: executor prompt does NOT forbid file-modifying commands");
    check(executor.system_prompt.find("Never `git add .` or `git add -A`")
              != std::string::npos,
          "S8c: executor prompt forbids git add . / -A (stage explicit paths)");
    check(executor.system_prompt.find("**Build and test what you write.**")
              != std::string::npos,
          "S8c: executor prompt orders build + test");
    check(executor.system_prompt.find("SUMMARY, OBSERVATION, HANDOFF, VAULT_UPDATE")
              != std::string::npos,
          "S8c: executor prompt names its structured blocks");
    // Doers DO emit VAULT_UPDATE (own-vault; see the doer SKILL's brainstorm +
    // 'Consult Vault Inventory' sections, and main.cpp's apply site, which has
    // no role gate), so the own-vault scoping rule is kept for executors.
    check(executor.system_prompt.find(
              "**Only write to YOUR vault.** VAULT_UPDATE paths must start with")
              != std::string::npos,
          "S8c: executor prompt keeps the own-vault VAULT_UPDATE scoping rule");

    // --- the VERDICT sentence is byte-identical in both variants ------------
    check(analyst.system_prompt.find(kVerdictForm) != std::string::npos
              && executor.system_prompt.find(kVerdictForm) != std::string::npos,
          "S8d: the VERDICT rule is present, identical, in BOTH variants");
    check(analyst.system_prompt.find("5. **End with a one-line verdict.**")
              != std::string::npos,
          "S8d: analyst numbers the verdict rule 5");
    check(executor.system_prompt.find("6. **End with a one-line verdict.**")
              != std::string::npos,
          "S8d: executor numbers the verdict rule 6");

    // --- brainstorm: a doer never gets the executor block -------------------
    // Decision #52 hard-rejects any HANDOFF resolving to a doer in brainstorm,
    // and the invoker clamps every agent to Read/Grep/Glob there. The assembler
    // does not itself reject the call (unchanged behaviour) — it emits the
    // ANALYST block, so the prompt matches the clamped tool surface.
    auto brainstorm_doer = assembler.assemble_split(
        "agent-s8-brain-doer", vault, "turn", "task S8 brainstorm doer",
        /*team_roster=*/{}, /*skill_file=*/{}, /*project_root=*/{},
        /*agent_role=*/"doer", /*budget=*/{}, /*conversation_mode=*/"brainstorm");
    check(brainstorm_doer.system_prompt.find(kExecTargetDir) == std::string::npos,
          "S8e: brainstorm + role doer -> executor block ABSENT");
    check(extract_output_rules(brainstorm_doer.system_prompt) == kAnalystRulesGolden,
          "S8e: brainstorm + role doer -> the analyst block, byte-identical");

    // --- explicit agent_class threads through assemble_split ----------------
    auto forced_analyst = assembler.assemble_split(
        "agent-s8-forced", vault, "turn", "task S8 forced",
        /*team_roster=*/{}, /*skill_file=*/{}, /*project_root=*/{},
        /*agent_role=*/"doer", /*budget=*/{}, /*conversation_mode=*/{},
        /*agent_class=*/"analyst");
    check(extract_output_rules(forced_analyst.system_prompt) == kAnalystRulesGolden,
          "S8f: explicit agent_class=analyst on a doer yields the analyst block");

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
    test_s7_verdict_rule_in_output_rules();
    test_s8_output_rules_are_class_aware();

    std::cout << "\n---------------------------------------------------\n";
    std::cout << "  passed: " << g_passed << "  failed: " << g_failed << "\n";
    std::cout << "---------------------------------------------------\n";

    return g_failed == 0 ? 0 : 1;
}
