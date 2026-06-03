// tests/integration/test_team_pipeline.cpp
// Integration tests for the team mode pipeline — raw output strings parsed
// through the real OutputParser, fed into the real ConversationEngine, with
// results verified in the real SQLite database.
//
// Run:  cd build && cmake .. && make test_team_pipeline && ./test_team_pipeline

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>
#include <sqlite3.h>

#include "storage/database.h"
#include "daemon/conversation.h"
#include "agent/output_parser.h"
#include "agent/context_assembler.h"
#include "utils/config.h"
#include "vault/vault_manager.h"

// ---- helpers ----------------------------------------------------------------

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

static void init_schema(sui::quorum::Database& db) {
    db.execute(
        "CREATE TABLE IF NOT EXISTS conversations ("
        "  id INTEGER PRIMARY KEY,"
        "  goal TEXT NOT NULL,"
        "  state TEXT NOT NULL DEFAULT 'active',"
        "  round INTEGER NOT NULL DEFAULT 0,"
        "  max_rounds INTEGER NOT NULL DEFAULT 3,"
        "  budget_usd REAL NOT NULL DEFAULT 5.0,"
        "  spent_usd REAL NOT NULL DEFAULT 0.0,"
        "  created_at TEXT NOT NULL DEFAULT (datetime('now')),"
        "  completed_at TEXT,"
        "  paused_reason TEXT,"
        "  current_agent TEXT,"
        "  path_index INTEGER NOT NULL DEFAULT 0,"
        "  team TEXT,"
        "  mode TEXT NOT NULL DEFAULT 'generic',"
        "  no_vault_write INTEGER NOT NULL DEFAULT 0"
        ")"
    );
    db.execute(
        "CREATE TABLE IF NOT EXISTS tasks ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  agent TEXT NOT NULL,"
        "  task_type TEXT NOT NULL,"
        "  status TEXT NOT NULL DEFAULT 'pending',"
        "  prompt TEXT NOT NULL,"
        "  result TEXT,"
        "  token_in INTEGER,"
        "  token_out INTEGER,"
        "  cost REAL,"
        "  error TEXT,"
        "  created_at TEXT NOT NULL DEFAULT (datetime('now')),"
        "  started_at TEXT,"
        "  completed_at TEXT,"
        "  conversation_id INTEGER REFERENCES conversations(id),"
        "  session_id TEXT,"
        "  system_prompt TEXT,"  // Phase 7 Track 5
        "  cache_creation_input_tokens INTEGER,"
        "  cache_read_input_tokens INTEGER"
        ")"
    );
    db.execute(
        "CREATE TABLE IF NOT EXISTS agent_sessions ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  cycle_id    INTEGER NOT NULL REFERENCES conversations(id),"
        "  agent_id    TEXT NOT NULL,"
        "  session_id  TEXT NOT NULL,"
        "  UNIQUE(cycle_id, agent_id)"
        ")"
    );
}

// ---- test harness -----------------------------------------------------------

struct TestHarness {
    sui::quorum::Database db;
    sui::quorum::ConversationConfig cfg;
    std::vector<sui::quorum::AgentMetadata> agents;
    sui::quorum::OutputParser parser;

    TestHarness() : db(":memory:") {
        init_schema(db);
        cfg.leader = "leader";
        cfg.default_max_rounds = 20;
        cfg.default_budget_usd = 5.0;

        agents.push_back(sui::quorum::AgentMetadata{
            .id = "leader", .name = "Leader",
            .description = "Coordinates the team", .role = "leader"
        });
        agents.push_back(sui::quorum::AgentMetadata{
            .id = "thinker", .name = "Thinker",
            .description = "Plans and designs", .role = "thinker"
        });
        agents.push_back(sui::quorum::AgentMetadata{
            .id = "doer", .name = "Doer",
            .description = "Implements solutions", .role = "doer"
        });
        agents.push_back(sui::quorum::AgentMetadata{
            .id = "recap", .name = "Recap",
            .description = "Surveys what changed + where we left off", .role = "thinker"
        });
    }

    sui::quorum::ConversationEngine make_engine() {
        return sui::quorum::ConversationEngine(db, cfg, agents);
    }

    sui::quorum::ConversationEngine make_engine_with_assembler(
            const sui::quorum::ContextAssembler* asm_ptr) {
        return sui::quorum::ConversationEngine(db, cfg, agents, asm_ptr);
    }

    // ---- task query helpers ----

    struct TaskInfo {
        int64_t id{0};
        std::string agent;
        std::string prompt;
        std::string session_id;
    };

    TaskInfo get_pending_task(int64_t conv_id) {
        TaskInfo info;
        db.query(
            "SELECT id, agent, prompt, session_id FROM tasks "
            "WHERE conversation_id = ? AND status = 'pending' ORDER BY id DESC LIMIT 1",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conv_id);
            },
            [&](sqlite3_stmt* stmt) {
                info.id = sqlite3_column_int64(stmt, 0);
                auto a = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (a) info.agent = a;
                auto p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                if (p) info.prompt = p;
                auto s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                if (s) info.session_id = s;
            }
        );
        return info;
    }

    void complete_task(int64_t task_id) {
        db.execute(
            "UPDATE tasks SET status = 'done', completed_at = datetime('now') WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, task_id);
            }
        );
    }

    int64_t count_pending(int64_t conv_id) {
        return db.query_int(
            "SELECT COUNT(*) FROM tasks WHERE conversation_id = " +
            std::to_string(conv_id) + " AND status = 'pending'"
        );
    }

    int64_t count_all_tasks(int64_t conv_id) {
        return db.query_int(
            "SELECT COUNT(*) FROM tasks WHERE conversation_id = " +
            std::to_string(conv_id)
        );
    }

    std::vector<TaskInfo> get_all_tasks(int64_t conv_id) {
        std::vector<TaskInfo> tasks;
        db.query(
            "SELECT id, agent, prompt, session_id FROM tasks "
            "WHERE conversation_id = ? ORDER BY id",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conv_id);
            },
            [&](sqlite3_stmt* stmt) {
                TaskInfo r;
                r.id = sqlite3_column_int64(stmt, 0);
                auto a = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (a) r.agent = a;
                auto p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                if (p) r.prompt = p;
                auto s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                if (s) r.session_id = s;
                tasks.push_back(r);
            }
        );
        return tasks;
    }

    std::string get_paused_reason(int64_t conv_id) {
        std::string reason;
        db.query(
            "SELECT paused_reason FROM conversations WHERE id = ?",
            [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, conv_id); },
            [&](sqlite3_stmt* stmt) {
                auto r = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (r) reason = r;
            }
        );
        return reason;
    }
};

// ---- simulate_turn helper ---------------------------------------------------
// Parses raw output through the real OutputParser, marks the task done, then
// calls on_task_complete on the real ConversationEngine.

struct SimResult {
    sui::quorum::ParsedOutput parsed;
    bool still_active;
};

static SimResult simulate_turn(TestHarness& h,
                                sui::quorum::ConversationEngine& engine,
                                int64_t task_id,
                                const std::string& raw_output,
                                double cost) {
    auto parsed = h.parser.parse(raw_output);
    h.complete_task(task_id);
    bool active = engine.on_task_complete(task_id, parsed, cost);
    return {std::move(parsed), active};
}

// ---- Test A: Full default path cycle (happy path) ---------------------------

static void test_A_full_default_path_cycle() {
    std::cout << "\n=== A. Full Default Path Cycle ===\n\n";

    namespace fs = std::filesystem;

    TestHarness h;
    h.cfg.default_path = {"leader", "thinker", "doer", "recap"};

    // Set up tmp project root with a phase plan so the daemon's deterministic
    // checkoff backstop can flip a checkbox when the cycle completes.
    auto tdir = fs::temp_directory_path() /
        ("quorum_test_A_" + std::to_string(::getpid()));
    fs::remove_all(tdir);
    fs::create_directories(tdir / ".quorum");
    auto plan_path = tdir / "phase-1-plan.md";
    {
        std::ofstream f(plan_path, std::ios::trunc);
        f << "# Phase 1\n"
          << "\n"
          << "- [ ] Task 1: build it\n";
    }
    {
        std::ofstream f(tdir / ".quorum" / "current_phase.md", std::ios::trunc);
        f << plan_path.string() << "\n";
    }
    h.cfg.target_dir = tdir.string();

    auto engine = h.make_engine();

    auto conv_id = engine.start("Build a REST API", 5.0, 20);

    // Turn 1: leader HANDOFFs to thinker carrying the "Task 1:" prefix
    auto t1 = h.get_pending_task(conv_id);
    std::string leader_out =
        "Plan ready.\n"
        "\n"
        "```HANDOFF\n"
        "to: thinker\n"
        "prompt: Task 1: build it\n"
        "```\n";
    simulate_turn(h, engine, t1.id, leader_out, 0.10);

    // Turn 2: thinker preserves Task 1 prefix
    auto t2 = h.get_pending_task(conv_id);
    std::string thinker_out =
        "Plan complete.\n"
        "\n"
        "```HANDOFF\n"
        "to: doer\n"
        "prompt: Task 1: build it\n"
        "```\n";
    simulate_turn(h, engine, t2.id, thinker_out, 0.10);

    // Turn 3: doer preserves Task 1 prefix
    auto t3 = h.get_pending_task(conv_id);
    std::string doer_out =
        "Implementation done.\n"
        "\n"
        "```HANDOFF\n"
        "to: recap\n"
        "prompt: Task 1: build it\n"
        "```\n";
    simulate_turn(h, engine, t3.id, doer_out, 0.10);

    // Turn 4: recap -> done
    auto t4 = h.get_pending_task(conv_id);
    std::string recap_out =
        "Recap survey complete.\n"
        "\n"
        "```HANDOFF\n"
        "to: done\n"
        "```\n";
    simulate_turn(h, engine, t4.id, recap_out, 0.10);

    // Verify task count and order
    check(h.count_all_tasks(conv_id) == 4, "A: 4 tasks total");
    auto tasks = h.get_all_tasks(conv_id);
    check(tasks[0].agent == "leader",  "A: task 1 == leader");
    check(tasks[1].agent == "thinker", "A: task 2 == thinker");
    check(tasks[2].agent == "doer",    "A: task 3 == doer");
    check(tasks[3].agent == "recap",   "A: task 4 == recap");

    // Verify final state
    auto conv = h.db.get_conversation(conv_id);
    check(conv->state == "done", "A: final state == done");
    check(conv->round == 4, "A: round == 4");

    // Verify the deterministic checkoff flipped the plan line
    std::string plan_contents;
    {
        std::ifstream f(plan_path);
        std::ostringstream oss;
        oss << f.rdbuf();
        plan_contents = oss.str();
    }
    check(plan_contents.find("[x]") != std::string::npos,
          "A: phase plan now contains [x] (checkoff backstop fired)");
    check(plan_contents.find("- [x] Task 1: build it") != std::string::npos,
          "A: Task 1 line is checked");

    fs::remove_all(tdir);
}

// ---- Test B: HANDOFF override mid-path --------------------------------------

static void test_B_handoff_override_mid_path() {
    std::cout << "\n=== B. HANDOFF Override Mid-Path ===\n\n";

    TestHarness h;
    h.cfg.default_path = {"leader", "thinker", "doer", "recap"};
    auto engine = h.make_engine();

    auto conv_id = engine.start("Build a web app", 5.0, 20);

    // Leader: HANDOFF to doer, skipping thinker
    auto t1 = h.get_pending_task(conv_id);
    std::string leader_output =
        "I've reviewed the goal. Let's skip planning.\n"
        "\n"
        "```HANDOFF\n"
        "to: doer\n"
        "prompt: skip planning, just build it\n"
        "```\n";
    auto r1 = simulate_turn(h, engine, t1.id, leader_output, 0.10);
    check(r1.still_active, "B: active after leader HANDOFF");

    // Doer should have been created with the HANDOFF prompt
    auto t2 = h.get_pending_task(conv_id);
    check(t2.agent == "doer", "B: doer task created");
    check(t2.prompt == "skip planning, just build it", "B: doer prompt from HANDOFF");

    // Doer: HANDOFF to done
    std::string doer_output =
        "Done building.\n"
        "\n"
        "```HANDOFF\n"
        "to: done\n"
        "```\n";
    auto r2 = simulate_turn(h, engine, t2.id, doer_output, 0.10);
    check(!r2.still_active, "B: not active after done");

    auto conv = h.db.get_conversation(conv_id);
    check(conv->state == "done", "B: state == done");
    check(h.count_all_tasks(conv_id) == 2, "B: total 2 tasks");
}

// ---- Test C: Human interaction mid-flow -------------------------------------

static void test_C_human_interaction_mid_flow() {
    std::cout << "\n=== C. Human Interaction Mid-Flow ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    auto conv_id = engine.start("Build something", 5.0, 20);

    // Leader: HANDOFF to human
    auto t1 = h.get_pending_task(conv_id);
    std::string leader_session = t1.session_id;
    std::string leader_output =
        "I need to ask the user something.\n"
        "\n"
        "```HANDOFF\n"
        "to: human\n"
        "prompt: Which language should we use?\n"
        "```\n";
    simulate_turn(h, engine, t1.id, leader_output, 0.10);

    auto conv = h.db.get_conversation(conv_id);
    check(conv->state == "waiting_for_human", "C: state == waiting_for_human");

    // Human responds
    bool ok = engine.respond(conv_id, "Use Rust");
    check(ok, "C: respond returned true");

    conv = h.db.get_conversation(conv_id);
    check(conv->state == "active", "C: state == active after respond");

    // Verify leader gets the task back with same session
    auto t2 = h.get_pending_task(conv_id);
    check(t2.agent == "leader", "C: task for leader after respond");
    check(t2.session_id == leader_session, "C: leader session_id preserved");

    // Leader hands off to doer
    std::string leader_output2 =
        "Got it, using Rust.\n"
        "\n"
        "```HANDOFF\n"
        "to: doer\n"
        "prompt: implement in Rust\n"
        "```\n";
    auto r2 = simulate_turn(h, engine, t2.id, leader_output2, 0.10);
    check(r2.still_active, "C: active after leader handoff to doer");

    auto t3 = h.get_pending_task(conv_id);
    check(t3.agent == "doer", "C: doer task created");
    check(t3.prompt == "implement in Rust", "C: doer prompt correct");
}

// ---- Test E: Roster appears in task prompts ---------------------------------

static void test_E_roster_in_prompts() {
    std::cout << "\n=== E. Roster Appears in Task Prompts ===\n\n";

    TestHarness h;
    h.cfg.default_path = {"leader", "thinker", "doer", "recap"};
    sui::quorum::ContextAssembler assembler;
    auto engine = h.make_engine_with_assembler(&assembler);

    auto conv_id = engine.start("Build something", 5.0, 20);

    // Inspect the leader's task prompt (stored in DB)
    auto t1 = h.get_pending_task(conv_id);
    auto prompt = t1.prompt;

    check(prompt.find("## Your Team") != std::string::npos,
          "E: prompt contains '## Your Team'");
    check(prompt.find("leader") != std::string::npos,
          "E: prompt contains 'leader'");
    check(prompt.find("thinker") != std::string::npos,
          "E: prompt contains 'thinker'");
    check(prompt.find("doer") != std::string::npos,
          "E: prompt contains 'doer'");
    check(prompt.find("recap") != std::string::npos,
          "E: prompt contains 'recap'");
    check(prompt.find("## Routing") != std::string::npos,
          "E: prompt contains '## Routing'");
    check(prompt.find("```HANDOFF") != std::string::npos,
          "E: prompt contains '```HANDOFF'");
}

// ---- Test F: Session IDs consistent within cycle ----------------------------

static void test_F_session_ids_consistent() {
    std::cout << "\n=== F. Session IDs Consistent ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    auto conv_id = engine.start("Build X", 5.0, 20);

    // Turn 1: leader -> doer
    auto t1 = h.get_pending_task(conv_id);
    std::string leader_session_1 = t1.session_id;
    check(!leader_session_1.empty(), "F: leader session 1 non-empty");

    std::string output1 =
        "```HANDOFF\n"
        "to: doer\n"
        "prompt: build step 1\n"
        "```\n";
    simulate_turn(h, engine, t1.id, output1, 0.10);

    // Turn 2: doer -> leader
    auto t2 = h.get_pending_task(conv_id);
    std::string doer_session_1 = t2.session_id;
    check(!doer_session_1.empty(), "F: doer session 1 non-empty");

    std::string output2 =
        "```HANDOFF\n"
        "to: leader\n"
        "prompt: review step 1\n"
        "```\n";
    simulate_turn(h, engine, t2.id, output2, 0.10);

    // Turn 3: leader -> doer (leader session must match turn 1)
    auto t3 = h.get_pending_task(conv_id);
    check(t3.session_id == leader_session_1,
          "F: leader session persists across turns");

    std::string output3 =
        "```HANDOFF\n"
        "to: doer\n"
        "prompt: build step 2\n"
        "```\n";
    simulate_turn(h, engine, t3.id, output3, 0.10);

    // Turn 4: doer -> done (doer session must match turn 2)
    auto t4 = h.get_pending_task(conv_id);
    check(t4.session_id == doer_session_1,
          "F: doer session persists across turns");
    check(leader_session_1 != doer_session_1,
          "F: leader and doer have different sessions");

    // Finish the conversation
    std::string output4 =
        "```HANDOFF\n"
        "to: done\n"
        "```\n";
    simulate_turn(h, engine, t4.id, output4, 0.10);
}

// ---- Test H: Max turns pause ------------------------------------------------

static void test_H_max_turns_pause() {
    std::cout << "\n=== H. Max Turns Pause ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    // max_rounds=3: start sets round=1, completions increment
    auto conv_id = engine.start("Build X", 5.0, 3);

    // Completion 1: round=1 < 3 -> active
    auto t1 = h.get_pending_task(conv_id);
    std::string output1 =
        "```HANDOFF\n"
        "to: thinker\n"
        "prompt: plan\n"
        "```\n";
    auto r1 = simulate_turn(h, engine, t1.id, output1, 0.01);
    check(r1.still_active, "H: active after completion 1");

    // Completion 2: round=2 < 3 -> active
    auto t2 = h.get_pending_task(conv_id);
    std::string output2 =
        "```HANDOFF\n"
        "to: doer\n"
        "prompt: build\n"
        "```\n";
    auto r2 = simulate_turn(h, engine, t2.id, output2, 0.01);
    check(r2.still_active, "H: active after completion 2");

    // Completion 3: round=3 >= 3 -> paused
    auto t3 = h.get_pending_task(conv_id);
    std::string output3 =
        "```HANDOFF\n"
        "to: recap\n"
        "prompt: survey what changed\n"
        "```\n";
    auto r3 = simulate_turn(h, engine, t3.id, output3, 0.01);
    check(!r3.still_active, "H: not active after max turns");

    auto conv = h.db.get_conversation(conv_id);
    check(conv->state == "paused", "H: state == paused");
    check(h.get_paused_reason(conv_id).find("turns") != std::string::npos,
          "H: paused_reason contains 'turns'");
}

// ---- Test I: Mixed SUMMARY + HANDOFF in one output --------------------------

static void test_I_mixed_blocks() {
    std::cout << "\n=== I. Mixed SUMMARY + HANDOFF ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    auto conv_id = engine.start("Analyze codebase", 5.0, 20);

    auto t1 = h.get_pending_task(conv_id);
    std::string output =
        "Here's my analysis.\n"
        "\n"
        "```SUMMARY\n"
        "Analyzed the codebase, found 3 issues.\n"
        "```\n"
        "\n"
        "```HANDOFF\n"
        "to: doer\n"
        "prompt: Fix the 3 memory leaks in handler.cpp\n"
        "```\n";

    auto r = simulate_turn(h, engine, t1.id, output, 0.10);

    // Verify parsed output
    check(r.parsed.summary == "Analyzed the codebase, found 3 issues.",
          "I: summary correct");
    check(r.parsed.handoff.has_value(), "I: handoff present");
    check(r.parsed.handoff->to == "doer", "I: handoff.to == doer");

    // Verify doer task created with correct prompt
    auto t2 = h.get_pending_task(conv_id);
    check(t2.agent == "doer", "I: doer task created");
    check(t2.prompt == "Fix the 3 memory leaks in handler.cpp",
          "I: doer prompt correct");
}

// ---- Test J: Empty output (no structured blocks) ----------------------------

static void test_J_empty_output() {
    std::cout << "\n=== J. Empty Output ===\n\n";

    // J1: with default_path -> follows path to next agent
    {
        TestHarness h;
        h.cfg.default_path = {"leader", "thinker", "doer"};
        auto engine = h.make_engine();

        auto conv_id = engine.start("Build X", 5.0, 20);
        auto t1 = h.get_pending_task(conv_id);
        auto r1 = simulate_turn(h, engine, t1.id,
            "I looked at the code but I'm not sure what to do.", 0.10);
        check(r1.still_active, "J1: active (follows default path)");

        auto t2 = h.get_pending_task(conv_id);
        check(t2.agent == "thinker", "J1: next agent == thinker");
    }

    // J2: without default_path -> conversation completes
    {
        TestHarness h;
        // No default_path set (empty vector)
        auto engine = h.make_engine();

        auto conv_id = engine.start("Build X", 5.0, 20);
        auto t1 = h.get_pending_task(conv_id);
        auto r1 = simulate_turn(h, engine, t1.id,
            "I looked at the code but I'm not sure what to do.", 0.10);
        check(!r1.still_active, "J2: not active (no path, no handoff)");

        auto conv = h.db.get_conversation(conv_id);
        check(conv->state == "done", "J2: state == done");
    }
}

// ---- Phase 6 Track 6 — brainstorm e2e helpers ------------------------------
//
// Replicates the daemon's main.cpp wiring: after parsing a turn's output,
// call vault_manager.apply_all_updates_with_context() with the emitting
// agent's role, the conversation mode, and the team roster. This is the
// exact path that would run in production, exercised here through the same
// VaultManager / OutputParser plumbing.

// RAII guard that wipes a temp directory at scope exit so test runs leave
// no debris in /tmp/ even on early-exit (check() calls std::exit(1) on fail).
struct TempDirGuard {
    std::filesystem::path dir;
    explicit TempDirGuard(std::filesystem::path d) : dir(std::move(d)) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
    }
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    TempDirGuard(const TempDirGuard&) = delete;
    TempDirGuard& operator=(const TempDirGuard&) = delete;
};

// Apply vault updates from a parsed turn through VaultManager, mirroring
// the wiring in src/main.cpp's task-completion handler. The emitting role
// is resolved from the harness's agent roster.
static size_t apply_vault_updates_for_turn(
        TestHarness& h,
        sui::quorum::VaultManager& vm,
        const std::string& emitting_agent_id,
        const std::string& mode,
        const sui::quorum::ParsedOutput& parsed)
{
    if (parsed.vault_updates.empty()) return 0;

    std::string emitting_role;
    for (const auto& a : h.agents) {
        if (a.id == emitting_agent_id) {
            emitting_role = a.role;
            break;
        }
    }
    return vm.apply_all_updates_with_context(
        emitting_agent_id, emitting_role, mode, h.agents, parsed.vault_updates);
}

// ---- Test #19 — End-to-end brainstorm without doer --------------------------
//
// Phase 14: brainstorm accumulation = the PARTICIPATING KNOWERS self-write
// their OWN lens's slice (own-vault), behind the human gate. There is no
// scribe and no cross-vault curator. This drives
// leader → thinker → reviewer → done in brainstorm mode, where the thinker and
// the reviewer EACH self-write their OWN vault before handing forward. Asserts
// each own-vault write lands, target_dir is unchanged, and no stray files
// appear outside the vaults.
static void test_brainstorm_e2e() {
    std::cout << "\n=== #19. Brainstorm E2E (knower self-write, no doer) ===\n\n";

    namespace fs = std::filesystem;

    auto tdir_path = fs::temp_directory_path() /
        ("quorum_test_brainstorm_e2e_" + std::to_string(::getpid()));
    TempDirGuard tdir_guard(tdir_path);
    auto tdir = tdir_guard.dir;

    // Roster: leader + two knowers (thinker + reviewer). No doer, no scribe.
    TestHarness h;
    h.agents.clear();
    h.agents.push_back(sui::quorum::AgentMetadata{
        .id = "leader", .name = "Leader",
        .description = "Coordinates the team", .role = "leader"
    });
    h.agents.push_back(sui::quorum::AgentMetadata{
        .id = "thinker", .name = "Thinker",
        .description = "Plans and designs", .role = "thinker"
    });
    h.agents.push_back(sui::quorum::AgentMetadata{
        .id = "reviewer", .name = "Reviewer",
        .description = "Critiques designs", .role = "thinker"
    });
    h.cfg.target_dir = tdir.string();

    // Initialize vault directories. VaultManager creates vaults under
    // <base>/vaults/<agent_id>/ — anchor the base at the .quorum/ subdirectory
    // of target_dir so vaults live INSIDE the project tree (matches the
    // daemon's actual layout).
    auto vault_base = tdir / ".quorum";
    fs::create_directories(vault_base);
    sui::quorum::VaultManager vm(vault_base.string());
    check(vm.init_vault("leader"),   "#19: init_vault leader");
    check(vm.init_vault("thinker"),  "#19: init_vault thinker");
    check(vm.init_vault("reviewer"), "#19: init_vault reviewer");

    // Drop a fake project file at the root and remember its bytes — the
    // post-cycle assertion uses byte equality as a "git diff is empty" proxy
    // (no need for an actual git repo).
    auto sample_path = tdir / "sample.txt";
    const std::string sample_original = "fake project source — do not touch\n";
    {
        std::ofstream f(sample_path, std::ios::trunc);
        f << sample_original;
    }

    auto engine = h.make_engine();
    auto conv_id = engine.start("Explore caching options",
                                /*budget=*/5.0, /*max_rounds=*/20,
                                /*team=*/"default", /*mode=*/"brainstorm");

    // Turn 1: leader → thinker
    auto t1 = h.get_pending_task(conv_id);
    check(t1.agent == "leader", "#19: turn 1 == leader");
    auto r1 = simulate_turn(h, engine, t1.id,
        "Kicking off brainstorm.\n"
        "\n"
        "```HANDOFF\n"
        "to: thinker\n"
        "prompt: brainstorm caching tradeoffs\n"
        "```\n", 0.05);
    apply_vault_updates_for_turn(h, vm, "leader", "brainstorm", r1.parsed);

    // Turn 2: thinker self-writes its OWN vault (own-shape path), then → reviewer.
    auto t2 = h.get_pending_task(conv_id);
    check(t2.agent == "thinker", "#19: turn 2 == thinker");
    std::string thinker_out =
        "TTL vs LRU; cache stampede risk noted.\n"
        "\n"
        "```VAULT_UPDATE\n"
        "path: knowledge/rule-cache-strategy.md\n"
        "content: |\n"
        "  Always cache reads, never cache writes\n"
        "```\n"
        "\n"
        "```HANDOFF\n"
        "to: reviewer\n"
        "prompt: critique the caching options\n"
        "```\n";
    auto r2 = simulate_turn(h, engine, t2.id, thinker_out, 0.05);
    auto thinker_applied = apply_vault_updates_for_turn(
        h, vm, "thinker", "brainstorm", r2.parsed);
    check(thinker_applied == 1, "#19: thinker self-wrote its own vault (1/1)");

    // Turn 3: reviewer self-writes its OWN vault (own-shape path), then → done.
    auto t3 = h.get_pending_task(conv_id);
    check(t3.agent == "reviewer", "#19: turn 3 == reviewer");
    std::string reviewer_out =
        "LRU wins for read-heavy workloads; TTL for write-heavy.\n"
        "\n"
        "```VAULT_UPDATE\n"
        "path: knowledge/ref-cache-tradeoffs.md\n"
        "content: |\n"
        "  TTL vs LRU comparison: ...\n"
        "```\n"
        "\n"
        "```HANDOFF\n"
        "to: done\n"
        "```\n";
    auto r3 = simulate_turn(h, engine, t3.id, reviewer_out, 0.05);
    auto reviewer_applied = apply_vault_updates_for_turn(
        h, vm, "reviewer", "brainstorm", r3.parsed);
    check(reviewer_applied == 1, "#19: reviewer self-wrote its own vault (1/1)");

    // ── Assertions ───────────────────────────────────────────────────────────

    // A19a: conversation reached state="done"
    auto conv = h.db.get_conversation(conv_id);
    check(conv->state == "done", "#19 A19a: conversation state == done");

    // A19b: thinker self-wrote into ITS OWN vault.
    auto thinker_file = vault_base / "vaults" / "thinker" /
                        "knowledge" / "rule-cache-strategy.md";
    check(fs::exists(thinker_file),
          "#19 A19b: thinker/knowledge/rule-cache-strategy.md exists (own-vault)");
    {
        std::ifstream in(thinker_file);
        std::string body{std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>()};
        check(body.find("Always cache reads, never cache writes") != std::string::npos,
              "#19 A19b: thinker file contains expected content");
    }

    // A19c: reviewer self-wrote into ITS OWN vault.
    auto reviewer_file = vault_base / "vaults" / "reviewer" /
                         "knowledge" / "ref-cache-tradeoffs.md";
    check(fs::exists(reviewer_file),
          "#19 A19c: reviewer/knowledge/ref-cache-tradeoffs.md exists (own-vault)");
    {
        std::ifstream in(reviewer_file);
        std::string body{std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>()};
        check(body.find("TTL vs LRU comparison") != std::string::npos,
              "#19 A19c: reviewer file contains expected content");
    }

    // A19d: target_dir/sample.txt is byte-identical to original
    {
        std::ifstream in(sample_path);
        std::string body{std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>()};
        check(body == sample_original,
              "#19 A19d: target_dir/sample.txt unchanged (git-diff-empty proxy)");
    }

    // A19e: only sample.txt and .quorum/ exist directly under target_dir
    {
        std::vector<std::string> entries;
        for (const auto& e : fs::directory_iterator(tdir)) {
            entries.push_back(e.path().filename().string());
        }
        std::sort(entries.begin(), entries.end());
        check(entries.size() == 2,
              "#19 A19e: target_dir has exactly 2 top-level entries");
        check(entries.size() == 2 &&
              entries[0] == ".quorum" && entries[1] == "sample.txt",
              "#19 A19e: target_dir contents are { .quorum, sample.txt }");
    }
}

// (Phase 14 removed the former Test #20 — "brainstorm with doer" — which
// exercised the retired scribe cross-vault exception AND a thinker→doer
// brainstorm HANDOFF. The daemon now HARD-REJECTS any brainstorm HANDOFF→doer
// (Phase 14 T1, covered by tests/unit/test_brainstorm_doer_reject.cpp), and
// cross-vault writes are rejected for all roles, so that scenario no longer
// reaches a doer turn.)

// ---- main -------------------------------------------------------------------

int main() {
    std::cout << "=== Team Mode Integration Tests ===\n";

    test_A_full_default_path_cycle();
    test_B_handoff_override_mid_path();
    test_C_human_interaction_mid_flow();
    test_E_roster_in_prompts();
    test_F_session_ids_consistent();
    test_H_max_turns_pause();
    test_I_mixed_blocks();
    test_J_empty_output();
    test_brainstorm_e2e();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
