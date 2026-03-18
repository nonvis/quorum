// tests/integration/test_team_pipeline.cpp
// Integration tests for the team mode pipeline — raw output strings parsed
// through the real OutputParser, fed into the real ConversationEngine, with
// results verified in the real SQLite database.
//
// Run:  cd build && cmake .. && make test_team_pipeline && ./test_team_pipeline

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "storage/database.h"
#include "daemon/conversation.h"
#include "agent/output_parser.h"
#include "agent/context_assembler.h"
#include "utils/config.h"

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
        "  path_index INTEGER NOT NULL DEFAULT 0"
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
        "  session_id TEXT"
        ")"
    );
    db.execute(
        "CREATE TABLE IF NOT EXISTS knowledge_ledger ("
        "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  cycle_id     INTEGER NOT NULL REFERENCES conversations(id),"
        "  agent_id     TEXT NOT NULL,"
        "  turn_number  INTEGER NOT NULL,"
        "  topic        TEXT,"
        "  content      TEXT NOT NULL,"
        "  created_at   TEXT NOT NULL DEFAULT (datetime('now'))"
        ")"
    );
    db.execute(
        "CREATE INDEX IF NOT EXISTS idx_knowledge_cycle ON knowledge_ledger(cycle_id)"
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
            .id = "scribe", .name = "Scribe",
            .description = "Documents findings", .role = "scribe"
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

    TestHarness h;
    h.cfg.default_path = {"leader", "thinker", "doer", "scribe"};
    auto engine = h.make_engine();

    auto conv_id = engine.start("Build a REST API", 5.0, 20);

    // Turn 1: leader (no HANDOFF -> follows default path)
    auto t1 = h.get_pending_task(conv_id);
    simulate_turn(h, engine, t1.id, "I analyzed the problem. Looks good.", 0.10);

    // Turn 2: thinker
    auto t2 = h.get_pending_task(conv_id);
    simulate_turn(h, engine, t2.id, "Planning complete. All looks fine.", 0.10);

    // Turn 3: doer
    auto t3 = h.get_pending_task(conv_id);
    simulate_turn(h, engine, t3.id, "Implementation done. Tests pass.", 0.10);

    // Turn 4: scribe -> end of path -> done
    auto t4 = h.get_pending_task(conv_id);
    simulate_turn(h, engine, t4.id, "Documentation complete.", 0.10);

    // Verify task count and order
    check(h.count_all_tasks(conv_id) == 4, "A: 4 tasks total");
    auto tasks = h.get_all_tasks(conv_id);
    check(tasks[0].agent == "leader",  "A: task 1 == leader");
    check(tasks[1].agent == "thinker", "A: task 2 == thinker");
    check(tasks[2].agent == "doer",    "A: task 3 == doer");
    check(tasks[3].agent == "scribe",  "A: task 4 == scribe");

    // Verify final state
    auto conv = h.db.get_conversation(conv_id);
    check(conv->state == "done", "A: final state == done");
    // start increments to 1, then 3 completions each increment (4th hits done)
    check(conv->round == 4, "A: round == 4");
}

// ---- Test B: HANDOFF override mid-path --------------------------------------

static void test_B_handoff_override_mid_path() {
    std::cout << "\n=== B. HANDOFF Override Mid-Path ===\n\n";

    TestHarness h;
    h.cfg.default_path = {"leader", "thinker", "doer", "scribe"};
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

// ---- Test D: Knowledge ledger accumulation ----------------------------------

static void test_D_knowledge_ledger_accumulation() {
    std::cout << "\n=== D. Knowledge Ledger Accumulation ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    auto conv_id = engine.start("Build a REST API", 5.0, 20);

    // Leader: KNOWLEDGE + HANDOFF to thinker
    auto t1 = h.get_pending_task(conv_id);
    std::string leader_output =
        "```KNOWLEDGE\n"
        "topic: goal\n"
        "content: Building a REST API\n"
        "```\n"
        "\n"
        "```HANDOFF\n"
        "to: thinker\n"
        "prompt: plan it\n"
        "```\n";
    auto r1 = simulate_turn(h, engine, t1.id, leader_output, 0.10);
    check(r1.still_active, "D: active after leader");

    // Thinker: KNOWLEDGE + HANDOFF to doer
    auto t2 = h.get_pending_task(conv_id);
    std::string thinker_output =
        "```KNOWLEDGE\n"
        "topic: architecture\n"
        "content: Using Express.js with PostgreSQL\n"
        "```\n"
        "\n"
        "```HANDOFF\n"
        "to: doer\n"
        "prompt: implement\n"
        "```\n";
    auto r2 = simulate_turn(h, engine, t2.id, thinker_output, 0.10);
    check(r2.still_active, "D: active after thinker");

    // Doer: KNOWLEDGE + HANDOFF to done
    auto t3 = h.get_pending_task(conv_id);
    std::string doer_output =
        "```KNOWLEDGE\n"
        "topic: implementation\n"
        "content: Created 3 endpoints, all tests pass\n"
        "```\n"
        "\n"
        "```HANDOFF\n"
        "to: done\n"
        "```\n";
    auto r3 = simulate_turn(h, engine, t3.id, doer_output, 0.10);
    check(!r3.still_active, "D: not active after done");

    // Verify knowledge ledger
    check(h.db.count_cycle_knowledge(conv_id) == 3, "D: knowledge count == 3");
    auto text = h.db.get_cycle_knowledge(conv_id);
    check(text.find("REST API") != std::string::npos,     "D: contains 'REST API'");
    check(text.find("Express.js") != std::string::npos,   "D: contains 'Express.js'");
    check(text.find("3 endpoints") != std::string::npos,  "D: contains '3 endpoints'");
}

// ---- Test E: Roster appears in task prompts ---------------------------------

static void test_E_roster_in_prompts() {
    std::cout << "\n=== E. Roster Appears in Task Prompts ===\n\n";

    TestHarness h;
    h.cfg.default_path = {"leader", "thinker", "doer", "scribe"};
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
    check(prompt.find("scribe") != std::string::npos,
          "E: prompt contains 'scribe'");
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

// ---- Test G: Budget pause stops routing -------------------------------------

static void test_G_budget_pause() {
    std::cout << "\n=== G. Budget Pause ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    auto conv_id = engine.start("Build X", 0.05, 20);

    // Turn 1: cost $0.03, total $0.03 < $0.05 -> still active
    auto t1 = h.get_pending_task(conv_id);
    std::string output1 =
        "```HANDOFF\n"
        "to: thinker\n"
        "prompt: plan it\n"
        "```\n";
    auto r1 = simulate_turn(h, engine, t1.id, output1, 0.03);
    check(r1.still_active, "G: active after $0.03 (budget $0.05)");

    // Turn 2: cost $0.03, total $0.06 >= $0.05 -> paused
    auto t2 = h.get_pending_task(conv_id);
    std::string output2 =
        "```HANDOFF\n"
        "to: doer\n"
        "prompt: build it\n"
        "```\n";
    auto r2 = simulate_turn(h, engine, t2.id, output2, 0.03);
    check(!r2.still_active, "G: not active after budget exceeded");

    auto conv = h.db.get_conversation(conv_id);
    check(conv->state == "paused", "G: state == paused");
    check(h.get_paused_reason(conv_id).find("budget") != std::string::npos,
          "G: paused_reason contains 'budget'");
    check(h.count_pending(conv_id) == 0, "G: no pending tasks after pause");
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
        "to: scribe\n"
        "prompt: document\n"
        "```\n";
    auto r3 = simulate_turn(h, engine, t3.id, output3, 0.01);
    check(!r3.still_active, "H: not active after max turns");

    auto conv = h.db.get_conversation(conv_id);
    check(conv->state == "paused", "H: state == paused");
    check(h.get_paused_reason(conv_id).find("turns") != std::string::npos,
          "H: paused_reason contains 'turns'");
}

// ---- Test I: Mixed SUMMARY + KNOWLEDGE + HANDOFF in one output --------------

static void test_I_mixed_blocks() {
    std::cout << "\n=== I. Mixed SUMMARY + KNOWLEDGE + HANDOFF ===\n\n";

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
        "```KNOWLEDGE\n"
        "topic: code-quality\n"
        "content: Found 3 potential memory leaks in the handler module.\n"
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
    check(static_cast<int>(r.parsed.knowledge.size()) == 1,
          "I: knowledge.size() == 1");
    check(r.parsed.handoff.has_value(), "I: handoff present");
    check(r.parsed.handoff->to == "doer", "I: handoff.to == doer");

    // Verify knowledge written to ledger
    check(h.db.count_cycle_knowledge(conv_id) == 1, "I: ledger count == 1");

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

// ---- main -------------------------------------------------------------------

int main() {
    std::cout << "=== Team Mode Integration Tests ===\n";

    test_A_full_default_path_cycle();
    test_B_handoff_override_mid_path();
    test_C_human_interaction_mid_flow();
    test_D_knowledge_ledger_accumulation();
    test_E_roster_in_prompts();
    test_F_session_ids_consistent();
    test_G_budget_pause();
    test_H_max_turns_pause();
    test_I_mixed_blocks();
    test_J_empty_output();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
