// tests/unit/test_escalation.cpp
// Unit tests for ConversationEngine pause conditions and escalation.
//
// Run:  cd build && cmake .. && make test_escalation && ./test_escalation

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include <sqlite3.h>

#include "storage/database.h"
#include "agent/output_parser.h"
#include "daemon/conversation.h"

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
        "  state TEXT NOT NULL DEFAULT 'init',"
        "  round INTEGER NOT NULL DEFAULT 0,"
        "  max_rounds INTEGER NOT NULL DEFAULT 3,"
        "  budget_usd REAL NOT NULL DEFAULT 5.0,"
        "  spent_usd REAL NOT NULL DEFAULT 0.0,"
        "  created_at TEXT NOT NULL DEFAULT (datetime('now')),"
        "  completed_at TEXT,"
        "  paused_reason TEXT"
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
}

static int64_t get_pending_task(sui::quorum::Database& db, int64_t conv_id,
                                const std::string& task_type) {
    int64_t task_id = 0;
    db.query(
        "SELECT id FROM tasks WHERE conversation_id = ? AND task_type = ? "
        "AND status = 'pending' ORDER BY id DESC LIMIT 1",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, conv_id);
            sqlite3_bind_text(stmt, 2, task_type.c_str(), -1, SQLITE_TRANSIENT);
        },
        [&](sqlite3_stmt* stmt) {
            task_id = sqlite3_column_int64(stmt, 0);
        }
    );
    return task_id;
}

static void mark_done(sui::quorum::Database& db, int64_t task_id) {
    db.execute(
        "UPDATE tasks SET status = 'done' WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, task_id);
        }
    );
}

static void mark_failed(sui::quorum::Database& db, int64_t task_id) {
    db.execute(
        "UPDATE tasks SET status = 'failed' WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, task_id);
        }
    );
}

static void set_token_in(sui::quorum::Database& db, int64_t task_id, int64_t tokens) {
    db.execute(
        "UPDATE tasks SET token_in = ? WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, tokens);
            sqlite3_bind_int64(stmt, 2, task_id);
        }
    );
}

// Helper: get conversation state
static std::string get_state(sui::quorum::Database& db, int64_t conv_id) {
    auto conv = db.get_conversation(conv_id);
    return conv ? conv->state : "";
}

// Helper: get paused_reason
static std::string get_paused_reason(sui::quorum::Database& db, int64_t conv_id) {
    std::string reason;
    db.query(
        "SELECT paused_reason FROM conversations WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, conv_id);
        },
        [&](sqlite3_stmt* stmt) {
            auto s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (s) reason = s;
        }
    );
    return reason;
}

// Helper: simulate think->review transition. Returns review task_id.
static int64_t simulate_think_to_review(sui::quorum::Database& db,
                                         sui::quorum::ConversationEngine& engine,
                                         int64_t conv_id,
                                         double cost = 0.10) {
    auto think_id = get_pending_task(db, conv_id, "think");
    mark_done(db, think_id);

    sui::quorum::ParsedOutput parsed;
    parsed.proposals.push_back({"Test proposal", {}, "content..."});
    engine.on_task_complete(think_id, parsed, cost);

    return get_pending_task(db, conv_id, "review");
}

// --- Test a: budget pause ---------------------------------------------------

static void test_budget_pause() {
    std::cout << "\n=== a. Budget Pause ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test budget", 0.50, 3);
    auto think_id = get_pending_task(db, conv_id, "think");
    mark_done(db, think_id);

    sui::quorum::ParsedOutput parsed;
    parsed.proposals.push_back({"Some proposal", {}, "content"});
    auto active = engine.on_task_complete(think_id, parsed, 0.60);

    check(!active, "a: on_task_complete returns false (paused)");
    check(get_state(db, conv_id) == "paused", "a: state == paused");
    check(get_paused_reason(db, conv_id).find("budget") != std::string::npos,
          "a: paused_reason contains 'budget'");
}

// --- Test b: token anomaly pause --------------------------------------------

static void test_token_anomaly_pause() {
    std::cout << "\n=== b. Token Anomaly Pause ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test tokens", 10.0, 3);

    // Insert 3 completed tasks with token_in=1000 to establish median
    for (int i = 0; i < 3; ++i) {
        db.execute(
            "INSERT INTO tasks (agent, task_type, status, prompt, conversation_id, token_in) "
            "VALUES ('thinker', 'think', 'done', 'history task', ?, 1000)",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conv_id);
            }
        );
    }

    // Get the pending think task from engine.start(), set its token_in=2500 (>2x 1000)
    auto think_id = get_pending_task(db, conv_id, "think");
    mark_done(db, think_id);
    set_token_in(db, think_id, 2500);

    sui::quorum::ParsedOutput parsed;
    parsed.proposals.push_back({"Anomaly proposal", {}, "content"});
    auto active = engine.on_task_complete(think_id, parsed, 0.10);

    check(!active, "b: on_task_complete returns false (paused)");
    check(get_state(db, conv_id) == "paused", "b: state == paused");
    check(get_paused_reason(db, conv_id).find("token anomaly") != std::string::npos,
          "b: paused_reason contains 'token anomaly'");
}

// --- Test c: token anomaly no pause below threshold -------------------------

static void test_token_anomaly_no_pause_below_threshold() {
    std::cout << "\n=== c. Token Anomaly No Pause Below Threshold ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test tokens OK", 10.0, 3);

    // Insert 3 completed tasks with token_in=1000
    for (int i = 0; i < 3; ++i) {
        db.execute(
            "INSERT INTO tasks (agent, task_type, status, prompt, conversation_id, token_in) "
            "VALUES ('thinker', 'think', 'done', 'history task', ?, 1000)",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conv_id);
            }
        );
    }

    // Get the pending think task, set token_in=1500 (<2x 1000 = 2000)
    auto think_id = get_pending_task(db, conv_id, "think");
    mark_done(db, think_id);
    set_token_in(db, think_id, 1500);

    sui::quorum::ParsedOutput parsed;
    parsed.proposals.push_back({"OK proposal", {}, "content"});
    auto active = engine.on_task_complete(think_id, parsed, 0.10);

    check(active, "c: on_task_complete returns true (not paused)");
    check(get_state(db, conv_id) == "reviewing", "c: state == reviewing (normal transition)");
}

// --- Test d: consecutive failures pause -------------------------------------

static void test_consecutive_failures_pause() {
    std::cout << "\n=== d. Consecutive Failures Pause ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test failures", 10.0, 3);

    // Get the think task from start(), mark it failed (don't call on_task_complete
    // yet — that would close the conversation since there are no proposals)
    auto think_id1 = get_pending_task(db, conv_id, "think");
    mark_failed(db, think_id1);

    // Insert a second failed task for this conversation
    db.execute(
        "INSERT INTO tasks (agent, task_type, status, prompt, conversation_id) "
        "VALUES ('thinker', 'think', 'failed', 'failed task 2', ?)",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, conv_id);
        }
    );
    int64_t failed_id2 = 0;
    db.query(
        "SELECT id FROM tasks WHERE conversation_id = ? ORDER BY id DESC LIMIT 1",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, conv_id);
        },
        [&](sqlite3_stmt* stmt) {
            failed_id2 = sqlite3_column_int64(stmt, 0);
        }
    );

    // Now call on_task_complete for the second failed task.
    // State is still "thinking" (no prior on_task_complete call changed it).
    // Last 2 tasks by id DESC: failed_id2 (failed), think_id1 (failed) → 2 failures.
    sui::quorum::ParsedOutput empty_parsed;
    auto active = engine.on_task_complete(failed_id2, empty_parsed, 0.05);

    check(!active, "d: on_task_complete returns false (paused)");
    check(get_state(db, conv_id) == "paused", "d: state == paused");
    check(get_paused_reason(db, conv_id).find("consecutive failures") != std::string::npos,
          "d: paused_reason contains 'consecutive failures'");
}

// --- Test e: escalate verdict pauses ----------------------------------------

static void test_escalate_verdict_pauses() {
    std::cout << "\n=== e. Escalate Verdict Pauses ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test escalation", 5.0, 3);

    // think → review
    auto review_id = simulate_think_to_review(db, engine, conv_id);
    check(review_id > 0, "e: review task created");

    mark_done(db, review_id);

    // Reviewer says "escalate"
    sui::quorum::ParsedOutput parsed;
    parsed.reviews.push_back({"", "escalate", "conflicting evidence"});
    auto active = engine.on_task_complete(review_id, parsed, 0.20);

    check(!active, "e: on_task_complete returns false (paused)");
    check(get_state(db, conv_id) == "paused", "e: state == paused");
    check(get_paused_reason(db, conv_id).find("agent escalation") != std::string::npos,
          "e: paused_reason contains 'agent escalation'");
}

// --- Test f: escalate in handle_reviewing -----------------------------------

static void test_escalate_in_handle_reviewing() {
    std::cout << "\n=== f. Escalate in handle_reviewing ===\n\n";

    // This test confirms the escalate verdict flows through both the
    // pre-routing check in on_task_complete AND the handle_reviewing branch.
    // Since the pre-routing check fires first, the result is the same.

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test escalation handle_reviewing", 5.0, 3);

    auto review_id = simulate_think_to_review(db, engine, conv_id);
    check(review_id > 0, "f: review task created");

    mark_done(db, review_id);

    sui::quorum::ParsedOutput parsed;
    parsed.reviews.push_back({"", "Escalate", "needs human judgment"});
    auto active = engine.on_task_complete(review_id, parsed, 0.15);

    check(!active, "f: on_task_complete returns false (paused)");
    check(get_state(db, conv_id) == "paused", "f: state == paused");

    auto reason = get_paused_reason(db, conv_id);
    check(reason.find("escalation") != std::string::npos,
          "f: paused_reason contains 'escalation'");
}

// --- Test g: normal flow unaffected (regression) ----------------------------

static void test_normal_flow_unaffected() {
    std::cout << "\n=== g. Normal Flow Unaffected (regression) ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test normal flow", 5.0, 3);

    // think -> review (cost 0.30)
    auto review_id = simulate_think_to_review(db, engine, conv_id, 0.30);
    check(review_id > 0, "g: review task created");

    mark_done(db, review_id);

    // Reviewer approves (cost 0.20)
    sui::quorum::ParsedOutput parsed;
    parsed.reviews.push_back({"", "approve", "looks good"});
    auto active = engine.on_task_complete(review_id, parsed, 0.20);

    check(!active, "g: on_task_complete returns false (terminal — done)");
    check(get_state(db, conv_id) == "done", "g: state == done");

    // Verify total spent
    auto conv = db.get_conversation(conv_id);
    check(conv.has_value(), "g: conversation exists");
    check(std::abs(conv->spent_usd - 0.50) < 0.01, "g: spent_usd ~= 0.50");
}

// --- Test h: no anomaly with no history -------------------------------------

static void test_no_anomaly_with_no_history() {
    std::cout << "\n=== h. No Anomaly With No History ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test first task tokens", 10.0, 3);

    auto think_id = get_pending_task(db, conv_id, "think");
    mark_done(db, think_id);
    set_token_in(db, think_id, 5000);  // high tokens but no history to compare

    sui::quorum::ParsedOutput parsed;
    parsed.proposals.push_back({"First proposal", {}, "content"});
    auto active = engine.on_task_complete(think_id, parsed, 0.10);

    // Median is based on the single task with token_in=5000.
    // median = 5000, current = 5000, 5000 > 5000*2 is false. No pause.
    check(active, "h: on_task_complete returns true (not paused)");
    check(get_state(db, conv_id) == "reviewing",
          "h: state == reviewing (normal transition, no false positive)");
}

// --- main -------------------------------------------------------------------

int main() {
    std::cout << "=== Escalation & Pause Condition Tests ===\n";

    test_budget_pause();
    test_token_anomaly_pause();
    test_token_anomaly_no_pause_below_threshold();
    test_consecutive_failures_pause();
    test_escalate_verdict_pauses();
    test_escalate_in_handle_reviewing();
    test_normal_flow_unaffected();
    test_no_anomaly_with_no_history();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
