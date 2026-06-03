// tests/unit/test_brainstorm_doer_reject.cpp
// Phase 14 Track 1 (Decision L2) — brainstorm hard-rejects doers.
//
// In brainstorm mode the project is strictly read-only. If a resolved HANDOFF
// target is a `role: doer` agent, the daemon must ABORT the conversation (not
// silently clamp the doer's tools read-only) and print the L2 rejection
// message. This verifies the guard in conversation.h on_task_complete().
//
// Run:  cd build && cmake .. && make test_brainstorm_doer_reject \
//         && ./test_brainstorm_doer_reject

#include <cstdlib>
#include <iostream>
#include <string>

#include <sqlite3.h>

#include "storage/database.h"
#include "daemon/conversation.h"
#include "agent/output_parser.h"
#include "utils/config.h"

// ─── helpers (mirrors test_generic_loop.cpp) ─────────────────────────────────

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
        "  system_prompt TEXT,"
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

struct TestHarness {
    sui::quorum::Database db;
    sui::quorum::ConversationConfig cfg;
    std::vector<sui::quorum::AgentMetadata> agents;

    TestHarness() : db(":memory:") {
        init_schema(db);
        cfg.leader = "leader";
        cfg.default_max_rounds = 20;
        cfg.default_budget_usd = 5.0;

        // Roles matter here: the brainstorm guard keys off role == "doer".
        agents.push_back(sui::quorum::AgentMetadata{.id = "leader", .role = "leader"});
        agents.push_back(sui::quorum::AgentMetadata{.id = "thinker", .role = "thinker"});
        agents.push_back(sui::quorum::AgentMetadata{.id = "doer", .role = "doer"});
    }

    sui::quorum::ConversationEngine make_engine() {
        return sui::quorum::ConversationEngine(db, cfg, agents);
    }

    void complete_task(int64_t task_id) {
        db.execute(
            "UPDATE tasks SET status = 'done', completed_at = datetime('now') WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, task_id);
            }
        );
    }

    int64_t latest_pending_task(int64_t conv_id) {
        return db.query_int(
            "SELECT COALESCE(MAX(id), 0) FROM tasks WHERE conversation_id = " +
            std::to_string(conv_id) + " AND status = 'pending'"
        );
    }

    int64_t count_pending(int64_t conv_id) {
        return db.query_int(
            "SELECT COUNT(*) FROM tasks WHERE conversation_id = " +
            std::to_string(conv_id) + " AND status = 'pending'"
        );
    }

    int64_t count_doer_tasks(int64_t conv_id) {
        return db.query_int(
            "SELECT COUNT(*) FROM tasks WHERE conversation_id = " +
            std::to_string(conv_id) + " AND agent = 'doer'"
        );
    }
};

// ─── Test A: brainstorm + HANDOFF→doer ⇒ rejected, no dispatch, done ──────────

static void test_brainstorm_rejects_doer_handoff() {
    std::cout << "\n=== A. Brainstorm Rejects Doer HANDOFF ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    // Start in brainstorm mode.
    auto conv_id = engine.start("Discuss the design", 5.0, 20, "brainstorm");
    auto conv0 = h.db.get_conversation(conv_id);
    check(conv0 && conv0->mode == "brainstorm", "A: conversation mode == brainstorm");

    auto leader_task = h.latest_pending_task(conv_id);
    check(leader_task > 0, "A: leader task created");
    h.complete_task(leader_task);

    // Leader hands off to the doer (role: doer).
    sui::quorum::ParsedOutput parsed;
    parsed.handoff = sui::quorum::HandoffBlock{.to = "doer", .prompt = "implement X"};

    bool active = engine.on_task_complete(leader_task, parsed, 0.10);

    // Guard must abort: not active, conversation done, NO doer task dispatched.
    check(!active, "A: conversation NOT active after doer-in-brainstorm");

    auto conv = h.db.get_conversation(conv_id);
    check(conv && conv->state == "done",
          "A: conversation ended in terminal 'done' state");
    check(h.count_doer_tasks(conv_id) == 0,
          "A: NO doer task was dispatched");
    check(h.count_pending(conv_id) == 0,
          "A: no pending tasks remain");
}

// ─── Test B: control — generic mode still dispatches the doer ─────────────────

static void test_generic_still_dispatches_doer() {
    std::cout << "\n=== B. Generic Mode Still Dispatches Doer (control) ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    // Default (generic) mode — the guard must NOT fire.
    auto conv_id = engine.start("Build X", 5.0, 20);
    auto conv0 = h.db.get_conversation(conv_id);
    check(conv0 && conv0->mode == "generic", "B: conversation mode == generic");

    auto leader_task = h.latest_pending_task(conv_id);
    h.complete_task(leader_task);

    sui::quorum::ParsedOutput parsed;
    parsed.handoff = sui::quorum::HandoffBlock{.to = "doer", .prompt = "implement X"};

    bool active = engine.on_task_complete(leader_task, parsed, 0.10);
    check(active, "B: still active after doer handoff in generic");
    check(h.count_doer_tasks(conv_id) == 1, "B: doer task WAS dispatched");

    auto conv = h.db.get_conversation(conv_id);
    check(conv && conv->state == "active", "B: conversation still active");
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Brainstorm Doer-Reject Tests (Phase 14 T1 / L2) ===\n";

    test_brainstorm_rejects_doer_handoff();
    test_generic_still_dispatches_doer();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
