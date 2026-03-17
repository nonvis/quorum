// tests/unit/test_generic_loop.cpp
// Unit tests for the team mode ConversationEngine generic loop.
//
// Run:  cd build && cmake .. && make test_generic_loop && ./test_generic_loop

#include <cstdlib>
#include <iostream>
#include <string>

#include <sqlite3.h>

#include "storage/database.h"
#include "daemon/conversation.h"
#include "agent/output_parser.h"
#include "utils/config.h"

// ─── helpers ─────────────────────────────────────────────────────────────────

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

struct TestHarness {
    sui::quorum::Database db;
    sui::quorum::ConversationConfig cfg;
    std::vector<sui::quorum::AgentMetadata> agents;

    TestHarness() : db(":memory:") {
        init_schema(db);
        cfg.leader = "leader";
        cfg.default_max_rounds = 20;
        cfg.default_budget_usd = 5.0;

        agents.push_back(sui::quorum::AgentMetadata{.id = "leader"});
        agents.push_back(sui::quorum::AgentMetadata{.id = "thinker"});
        agents.push_back(sui::quorum::AgentMetadata{.id = "doer"});
        agents.push_back(sui::quorum::AgentMetadata{.id = "scribe"});
    }

    sui::quorum::ConversationEngine make_engine() {
        return sui::quorum::ConversationEngine(db, cfg, agents);
    }

    // Helper: get the latest pending task for a conversation
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

    // Helper: mark a task as done
    void complete_task(int64_t task_id) {
        db.execute(
            "UPDATE tasks SET status = 'done', completed_at = datetime('now') WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, task_id);
            }
        );
    }

    // Helper: count pending tasks for a conversation
    int64_t count_pending(int64_t conv_id) {
        return db.query_int(
            "SELECT COUNT(*) FROM tasks WHERE conversation_id = " +
            std::to_string(conv_id) + " AND status = 'pending'"
        );
    }
};

// ─── Test A: Start creates leader task ───────────────────────────────────────

static void test_start_creates_leader_task() {
    std::cout << "\n=== A. Start Creates Leader Task ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    auto conv_id = engine.start("Build X", 5.0, 20);
    check(conv_id > 0, "A: conv_id > 0");

    auto task = h.get_pending_task(conv_id);
    check(task.id > 0, "A: pending task created");
    check(task.agent == "leader", "A: agent == leader");
    check(task.prompt.find("Build X") != std::string::npos, "A: prompt contains goal");
    check(!task.session_id.empty(), "A: session_id non-empty");

    auto conv = h.db.get_conversation(conv_id);
    check(conv->current_agent == "leader", "A: current_agent == leader");
    check(conv->round == 1, "A: round == 1 after start");
}

// ─── Test B: HANDOFF routing ─────────────────────────────────────────────────

static void test_handoff_routing() {
    std::cout << "\n=== B. HANDOFF Routing ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    auto conv_id = engine.start("Build X", 5.0, 20);
    auto task1 = h.get_pending_task(conv_id);
    h.complete_task(task1.id);

    sui::quorum::ParsedOutput parsed;
    parsed.handoff = sui::quorum::HandoffBlock{.to = "doer", .prompt = "implement X"};

    bool active = engine.on_task_complete(task1.id, parsed, 0.10);
    check(active, "B: still active after handoff");

    auto task2 = h.get_pending_task(conv_id);
    check(task2.agent == "doer", "B: next task agent == doer");
    check(task2.prompt == "implement X", "B: next task prompt == implement X");
}

// ─── Test C: HANDOFF to done ─────────────────────────────────────────────────

static void test_handoff_done() {
    std::cout << "\n=== C. HANDOFF to Done ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    auto conv_id = engine.start("Build X", 5.0, 20);
    auto task1 = h.get_pending_task(conv_id);
    h.complete_task(task1.id);

    sui::quorum::ParsedOutput parsed;
    parsed.handoff = sui::quorum::HandoffBlock{.to = "done"};

    bool active = engine.on_task_complete(task1.id, parsed, 0.10);
    check(!active, "C: not active after done");

    auto conv = h.db.get_conversation(conv_id);
    check(conv->state == "done", "C: state == done");
    check(h.count_pending(conv_id) == 0, "C: no pending tasks");
}

// ─── Test D: HANDOFF to human ────────────────────────────────────────────────

static void test_handoff_human() {
    std::cout << "\n=== D. HANDOFF to Human ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    auto conv_id = engine.start("Build X", 5.0, 20);
    auto task1 = h.get_pending_task(conv_id);
    h.complete_task(task1.id);

    sui::quorum::ParsedOutput parsed;
    parsed.handoff = sui::quorum::HandoffBlock{
        .to = "human", .prompt = "need clarification on requirements"
    };

    bool active = engine.on_task_complete(task1.id, parsed, 0.10);
    check(!active, "D: not active (waiting for human)");

    auto conv = h.db.get_conversation(conv_id);
    check(conv->state == "waiting_for_human", "D: state == waiting_for_human");
    check(conv->current_agent == "human", "D: current_agent == human");
}

// ─── Test E: Respond to human ────────────────────────────────────────────────

static void test_respond_to_human() {
    std::cout << "\n=== E. Respond to Human ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    // Setup: start → handoff to human
    auto conv_id = engine.start("Build X", 5.0, 20);
    auto task1 = h.get_pending_task(conv_id);
    h.complete_task(task1.id);

    sui::quorum::ParsedOutput parsed;
    parsed.handoff = sui::quorum::HandoffBlock{
        .to = "human", .prompt = "need clarification"
    };
    engine.on_task_complete(task1.id, parsed, 0.10);

    // Now respond
    bool ok = engine.respond(conv_id, "yes proceed");
    check(ok, "E: respond returned true");

    auto conv = h.db.get_conversation(conv_id);
    check(conv->state == "active", "E: state == active after respond");

    auto task2 = h.get_pending_task(conv_id);
    check(task2.agent == "leader", "E: task for leader");
    check(task2.prompt.find("yes proceed") != std::string::npos,
          "E: prompt contains response text");
}

// ─── Test F: Default path routing ────────────────────────────────────────────

static void test_default_path_routing() {
    std::cout << "\n=== F. Default Path Routing ===\n\n";

    TestHarness h;
    h.cfg.default_path = {"leader", "thinker", "doer", "scribe"};
    auto engine = h.make_engine();

    auto conv_id = engine.start("Build X", 5.0, 20);

    // Complete leader (path_index=0) → should go to thinker (index 1)
    auto task1 = h.get_pending_task(conv_id);
    h.complete_task(task1.id);

    sui::quorum::ParsedOutput empty;
    bool active = engine.on_task_complete(task1.id, empty, 0.10);
    check(active, "F: active after leader");

    auto task2 = h.get_pending_task(conv_id);
    check(task2.agent == "thinker", "F: next == thinker");

    // Complete thinker → doer
    h.complete_task(task2.id);
    active = engine.on_task_complete(task2.id, empty, 0.10);
    check(active, "F: active after thinker");

    auto task3 = h.get_pending_task(conv_id);
    check(task3.agent == "doer", "F: next == doer");
}

// ─── Test G: Default path end → done ─────────────────────────────────────────

static void test_default_path_end_done() {
    std::cout << "\n=== G. Default Path End → Done ===\n\n";

    TestHarness h;
    h.cfg.default_path = {"leader", "doer"};
    auto engine = h.make_engine();

    auto conv_id = engine.start("Build X", 5.0, 20);

    // Complete leader → doer
    auto task1 = h.get_pending_task(conv_id);
    h.complete_task(task1.id);
    sui::quorum::ParsedOutput empty;
    engine.on_task_complete(task1.id, empty, 0.10);

    // Complete doer → should be done (end of path)
    auto task2 = h.get_pending_task(conv_id);
    h.complete_task(task2.id);
    bool active = engine.on_task_complete(task2.id, empty, 0.10);
    check(!active, "G: not active at end of path");

    auto conv = h.db.get_conversation(conv_id);
    check(conv->state == "done", "G: state == done");
}

// ─── Test H: HANDOFF overrides default path ──────────────────────────────────

static void test_handoff_overrides_default_path() {
    std::cout << "\n=== H. HANDOFF Overrides Default Path ===\n\n";

    TestHarness h;
    h.cfg.default_path = {"leader", "thinker", "doer"};
    auto engine = h.make_engine();

    auto conv_id = engine.start("Build X", 5.0, 20);

    // Complete leader with HANDOFF to scribe (not next in path)
    auto task1 = h.get_pending_task(conv_id);
    h.complete_task(task1.id);

    sui::quorum::ParsedOutput parsed;
    parsed.handoff = sui::quorum::HandoffBlock{
        .to = "scribe", .prompt = "document everything"
    };
    engine.on_task_complete(task1.id, parsed, 0.10);

    auto task2 = h.get_pending_task(conv_id);
    check(task2.agent == "scribe", "H: next == scribe (not thinker)");
}

// ─── Test I: Unknown agent → leader ──────────────────────────────────────────

static void test_unknown_agent_fallback() {
    std::cout << "\n=== I. Unknown Agent → Leader ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    auto conv_id = engine.start("Build X", 5.0, 20);
    auto task1 = h.get_pending_task(conv_id);
    h.complete_task(task1.id);

    sui::quorum::ParsedOutput parsed;
    parsed.handoff = sui::quorum::HandoffBlock{
        .to = "nonexistent-agent", .prompt = "do something"
    };
    engine.on_task_complete(task1.id, parsed, 0.10);

    auto task2 = h.get_pending_task(conv_id);
    check(task2.agent == "leader", "I: fallback to leader for unknown agent");
}

// ─── Test J: Budget exceeded ─────────────────────────────────────────────────

static void test_budget_exceeded() {
    std::cout << "\n=== J. Budget Exceeded ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    auto conv_id = engine.start("Build X", 0.01, 20);  // tiny budget
    auto task1 = h.get_pending_task(conv_id);
    h.complete_task(task1.id);

    sui::quorum::ParsedOutput parsed;
    parsed.handoff = sui::quorum::HandoffBlock{.to = "doer", .prompt = "build it"};

    bool active = engine.on_task_complete(task1.id, parsed, 0.05);
    check(!active, "J: not active after budget exceeded");

    auto conv = h.db.get_conversation(conv_id);
    check(conv->state == "paused", "J: state == paused");

    // Check paused_reason
    std::string reason;
    h.db.query(
        "SELECT paused_reason FROM conversations WHERE id = ?",
        [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, conv_id); },
        [&](sqlite3_stmt* stmt) {
            auto r = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (r) reason = r;
        }
    );
    check(reason.find("budget") != std::string::npos, "J: paused_reason contains 'budget'");
}

// ─── Test K: Max turns exceeded ──────────────────────────────────────────────

static void test_max_turns_exceeded() {
    std::cout << "\n=== K. Max Turns Exceeded ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    // max_turns=2: start() → round=1, first on_task_complete → round=2 + creates task,
    // second on_task_complete → round=2 >= max=2 → pause
    auto conv_id = engine.start("Build X", 5.0, 2);

    // Turn 1 complete → creates turn 2
    auto task1 = h.get_pending_task(conv_id);
    h.complete_task(task1.id);
    sui::quorum::ParsedOutput parsed;
    parsed.handoff = sui::quorum::HandoffBlock{.to = "doer", .prompt = "build"};
    bool active = engine.on_task_complete(task1.id, parsed, 0.01);
    check(active, "K: active after turn 1");

    // Turn 2 complete → round=2, round >= max_rounds=2 → pause
    auto task2 = h.get_pending_task(conv_id);
    h.complete_task(task2.id);
    parsed.handoff = sui::quorum::HandoffBlock{.to = "scribe", .prompt = "write docs"};
    active = engine.on_task_complete(task2.id, parsed, 0.01);
    check(!active, "K: not active after max turns");

    auto conv = h.db.get_conversation(conv_id);
    check(conv->state == "paused", "K: state == paused");

    std::string reason;
    h.db.query(
        "SELECT paused_reason FROM conversations WHERE id = ?",
        [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, conv_id); },
        [&](sqlite3_stmt* stmt) {
            auto r = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (r) reason = r;
        }
    );
    check(reason.find("turns") != std::string::npos, "K: paused_reason contains 'turns'");
}

// ─── Test L: Session resume within cycle ─────────────────────────────────────

static void test_session_resume_within_cycle() {
    std::cout << "\n=== L. Session Resume Within Cycle ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    auto conv_id = engine.start("Build X", 5.0, 20);

    // Leader gets initial task with session S1
    auto task1 = h.get_pending_task(conv_id);
    std::string leader_session = task1.session_id;
    check(!leader_session.empty(), "L: leader has session");

    // Complete leader → doer
    h.complete_task(task1.id);
    sui::quorum::ParsedOutput parsed;
    parsed.handoff = sui::quorum::HandoffBlock{.to = "doer", .prompt = "build"};
    engine.on_task_complete(task1.id, parsed, 0.10);

    // Complete doer → back to leader
    auto task2 = h.get_pending_task(conv_id);
    h.complete_task(task2.id);
    parsed.handoff = sui::quorum::HandoffBlock{.to = "leader", .prompt = "review"};
    engine.on_task_complete(task2.id, parsed, 0.10);

    // Leader's new task should have the SAME session_id
    auto task3 = h.get_pending_task(conv_id);
    check(task3.agent == "leader", "L: third task is leader");
    check(task3.session_id == leader_session, "L: leader session_id persists across turns");

    // Verify agent_sessions table has one entry for leader
    int64_t session_count = h.db.query_int(
        "SELECT COUNT(*) FROM agent_sessions WHERE cycle_id = " +
        std::to_string(conv_id) + " AND agent_id = 'leader'"
    );
    check(session_count == 1, "L: exactly one session entry for leader");
}

// ─── Test M: Knowledge ledger writes ─────────────────────────────────────────

static void test_knowledge_ledger_writes() {
    std::cout << "\n=== M. Knowledge Ledger Writes ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    auto conv_id = engine.start("Build X", 5.0, 20);
    auto task1 = h.get_pending_task(conv_id);
    h.complete_task(task1.id);

    sui::quorum::ParsedOutput parsed;
    parsed.handoff = sui::quorum::HandoffBlock{.to = "doer", .prompt = "build"};
    parsed.knowledge.push_back(sui::quorum::KnowledgeBlock{
        .topic = "architecture", .content = "Decided on microservices"
    });
    parsed.knowledge.push_back(sui::quorum::KnowledgeBlock{
        .topic = "tech-stack", .content = "Using Rust for backend"
    });

    engine.on_task_complete(task1.id, parsed, 0.10);

    auto count = h.db.count_cycle_knowledge(conv_id);
    check(count == 2, "M: knowledge_ledger count == 2");

    auto text = h.db.get_cycle_knowledge(conv_id);
    check(text.find("microservices") != std::string::npos,
          "M: ledger contains 'microservices'");
    check(text.find("Rust") != std::string::npos,
          "M: ledger contains 'Rust'");
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Generic Loop (Team Mode) Tests ===\n";

    test_start_creates_leader_task();
    test_handoff_routing();
    test_handoff_done();
    test_handoff_human();
    test_respond_to_human();
    test_default_path_routing();
    test_default_path_end_done();
    test_handoff_overrides_default_path();
    test_unknown_agent_fallback();
    test_budget_exceeded();
    test_max_turns_exceeded();
    test_session_resume_within_cycle();
    test_knowledge_ledger_writes();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
