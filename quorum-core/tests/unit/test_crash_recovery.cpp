// tests/unit/test_crash_recovery.cpp
// Unit tests for crash recovery (stale task cleanup + conversation re-dispatch).
//
// Run:  cd build && cmake .. && make test_crash_recovery && ./test_crash_recovery

#include <cstdlib>
#include <iostream>
#include <string>

#include <sqlite3.h>

#include "storage/database.h"
#include "daemon/conversation.h"
#include "utils/config.h"

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
        "  no_vault_write INTEGER NOT NULL DEFAULT 0,"
        "  gated INTEGER NOT NULL DEFAULT 0,"
        "  gate_cleared INTEGER NOT NULL DEFAULT 0"
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

    // Helper: get task status
    std::string get_task_status(int64_t task_id) {
        std::string status;
        db.query(
            "SELECT status FROM tasks WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, task_id);
            },
            [&](sqlite3_stmt* stmt) {
                auto s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (s) status = s;
            }
        );
        return status;
    }

    // Helper: get task error
    std::string get_task_error(int64_t task_id) {
        std::string error;
        db.query(
            "SELECT error FROM tasks WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, task_id);
            },
            [&](sqlite3_stmt* stmt) {
                auto e = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (e) error = e;
            }
        );
        return error;
    }

    // Helper: count tasks by status for a conversation
    int64_t count_tasks_by_status(int64_t conv_id, const std::string& status) {
        int64_t count = 0;
        db.query(
            "SELECT COUNT(*) FROM tasks WHERE conversation_id = ? AND status = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conv_id);
                sqlite3_bind_text(stmt, 2, status.c_str(), -1, SQLITE_TRANSIENT);
            },
            [&](sqlite3_stmt* stmt) {
                count = sqlite3_column_int64(stmt, 0);
            }
        );
        return count;
    }
};

// --- Test A: Stale active task recovered — leader re-dispatched ---------------

static void test_stale_task_recovered() {
    std::cout << "\n=== A. Stale Active Task Recovered ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    auto conv_id = engine.start("test goal", 5.0, 20);
    auto task1 = h.get_pending_task(conv_id);
    check(task1.id > 0, "A: initial leader task created");

    // Simulate task becoming active (daemon picked it up)
    h.db.execute(
        "UPDATE tasks SET status = 'active' WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, task1.id);
        }
    );

    // Simulate crash recovery: mark the task failed (mimicking recover_stale_tasks)
    h.db.execute(
        "UPDATE tasks SET status = 'failed', "
        "error = 'interrupted by daemon restart', "
        "completed_at = datetime('now') WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, task1.id);
        }
    );

    // Call recover
    bool recovered = engine.recover(conv_id);
    check(recovered, "A: recover returned true");

    // Verify original task is failed
    check(h.get_task_status(task1.id) == "failed", "A: original task status == failed");

    // Verify a new pending task exists for leader
    auto new_task = h.get_pending_task(conv_id);
    check(new_task.id > 0, "A: new pending task created");
    check(new_task.id != task1.id, "A: new task is different from original");
    check(new_task.agent == "leader", "A: new task agent == leader");
    check(new_task.prompt.find("Recovery") != std::string::npos,
          "A: new task prompt contains 'Recovery'");
    check(new_task.prompt.find("leader") != std::string::npos,
          "A: new task prompt contains interrupted agent name");
}

// --- Test B: No recovery for closed conversation ------------------------------

static void test_no_recover_closed_conversation() {
    std::cout << "\n=== B. No Recovery for Closed Conversation ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    auto conv_id = engine.start("test goal", 5.0, 20);
    auto task1 = h.get_pending_task(conv_id);

    // Close the conversation
    engine.close(conv_id);

    auto conv = h.db.get_conversation(conv_id);
    check(conv->state == "closed", "B: conversation is closed");

    // Count tasks before recovery attempt
    auto pending_before = h.count_tasks_by_status(conv_id, "pending");

    // Attempt recovery — should return false
    bool recovered = engine.recover(conv_id);
    check(!recovered, "B: recover returned false for closed conversation");

    // No new tasks created
    auto pending_after = h.count_tasks_by_status(conv_id, "pending");
    check(pending_after == pending_before, "B: no new tasks created");
}

// --- Test C: Multiple conversations recovered independently -------------------

static void test_multiple_conversations_recovered() {
    std::cout << "\n=== C. Multiple Conversations Recovered ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    // Start two conversations
    auto conv1 = engine.start("goal 1", 5.0, 20);
    auto conv2 = engine.start("goal 2", 5.0, 20);

    // For each: get pending task, mark active, then mark failed
    auto task1 = h.get_pending_task(conv1);
    auto task2 = h.get_pending_task(conv2);

    for (auto tid : {task1.id, task2.id}) {
        h.db.execute(
            "UPDATE tasks SET status = 'active' WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, tid);
            }
        );
        h.db.execute(
            "UPDATE tasks SET status = 'failed', "
            "error = 'interrupted by daemon restart', "
            "completed_at = datetime('now') WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, tid);
            }
        );
    }

    // Recover each
    bool r1 = engine.recover(conv1);
    bool r2 = engine.recover(conv2);
    check(r1, "C: conv1 recovered");
    check(r2, "C: conv2 recovered");

    // Each has a new pending leader task
    auto new1 = h.get_pending_task(conv1);
    auto new2 = h.get_pending_task(conv2);
    check(new1.id > 0, "C: conv1 has new pending task");
    check(new2.id > 0, "C: conv2 has new pending task");
    check(new1.agent == "leader", "C: conv1 new task is leader");
    check(new2.agent == "leader", "C: conv2 new task is leader");
    check(new1.id != new2.id, "C: two new tasks are different IDs");
}

// --- Test D: Non-active conversation not recovered ----------------------------

static void test_done_conversation_not_recovered() {
    std::cout << "\n=== D. Done Conversation Not Recovered ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    auto conv_id = engine.start("test goal", 5.0, 20);
    auto task1 = h.get_pending_task(conv_id);

    // Mark task active then failed (simulating crash)
    h.db.execute(
        "UPDATE tasks SET status = 'active' WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, task1.id);
        }
    );
    h.db.execute(
        "UPDATE tasks SET status = 'failed', "
        "error = 'interrupted by daemon restart', "
        "completed_at = datetime('now') WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, task1.id);
        }
    );

    // Manually set conversation state to 'done'
    h.db.execute(
        "UPDATE conversations SET state = 'done' WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, conv_id);
        }
    );

    // Attempt recovery
    bool recovered = engine.recover(conv_id);
    check(!recovered, "D: recover returned false for done conversation");

    // No new pending tasks
    auto pending = h.count_tasks_by_status(conv_id, "pending");
    check(pending == 0, "D: no new pending tasks for done conversation");
}

// --- Test E: Recovery prompt contains interrupted agent name ------------------

static void test_recovery_prompt_contains_agent_name() {
    std::cout << "\n=== E. Recovery Prompt Contains Interrupted Agent ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();

    auto conv_id = engine.start("test goal", 5.0, 20);

    // Get and complete leader task with HANDOFF to doer
    auto task1 = h.get_pending_task(conv_id);
    h.complete_task(task1.id);

    sui::quorum::ParsedOutput parsed;
    parsed.handoff = sui::quorum::HandoffBlock{.to = "doer", .prompt = "do stuff"};
    engine.on_task_complete(task1.id, parsed, 0.01);

    // Get the new pending doer task
    auto task2 = h.get_pending_task(conv_id);
    check(task2.agent == "doer", "E: second task is doer");

    // Mark doer task active then failed (simulating crash mid-doer)
    h.db.execute(
        "UPDATE tasks SET status = 'active' WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, task2.id);
        }
    );
    h.db.execute(
        "UPDATE tasks SET status = 'failed', "
        "error = 'interrupted by daemon restart', "
        "completed_at = datetime('now') WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, task2.id);
        }
    );

    // Recover — leader should get a prompt mentioning "doer"
    bool recovered = engine.recover(conv_id);
    check(recovered, "E: recover returned true");

    auto new_task = h.get_pending_task(conv_id);
    check(new_task.agent == "leader", "E: recovery task is for leader");
    check(new_task.prompt.find("Recovery") != std::string::npos,
          "E: prompt contains 'Recovery'");
    check(new_task.prompt.find("doer") != std::string::npos,
          "E: prompt contains 'doer' (the interrupted agent)");
}

// --- main --------------------------------------------------------------------

int main() {
    std::cout << "=== Crash Recovery Tests ===\n";

    test_stale_task_recovered();
    test_no_recover_closed_conversation();
    test_multiple_conversations_recovered();
    test_done_conversation_not_recovered();
    test_recovery_prompt_contains_agent_name();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
