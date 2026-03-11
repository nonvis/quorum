// tests/unit/test_conversation.cpp
// Unit tests for ConversationEngine state machine.
//
// Run:  cd build && cmake .. && make test_conversation && ./test_conversation

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include <sqlite3.h>

#include "storage/database.h"
#include "agent/output_parser.h"
#include "daemon/conversation.h"

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
        "  state TEXT NOT NULL DEFAULT 'init',"
        "  round INTEGER NOT NULL DEFAULT 0,"
        "  max_rounds INTEGER NOT NULL DEFAULT 3,"
        "  budget_usd REAL NOT NULL DEFAULT 5.0,"
        "  spent_usd REAL NOT NULL DEFAULT 0.0,"
        "  created_at TEXT NOT NULL DEFAULT (datetime('now')),"
        "  completed_at TEXT,"
        "  paused_reason TEXT,"
        "  pipeline TEXT NOT NULL DEFAULT 'analyst'"
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

// Helper: get first pending task id for a conversation + task_type
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

// Helper: mark a task as done
static void mark_done(sui::quorum::Database& db, int64_t task_id) {
    db.execute(
        "UPDATE tasks SET status = 'done' WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, task_id);
        }
    );
}

// Helper: get session_id for a task
static std::string get_session_id(sui::quorum::Database& db, int64_t task_id) {
    std::string sid;
    db.query(
        "SELECT session_id FROM tasks WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, task_id);
        },
        [&](sqlite3_stmt* stmt) {
            auto s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (s) sid = s;
        }
    );
    return sid;
}

// Helper: simulate think→review transition. Returns review task_id.
static int64_t simulate_think_to_review(sui::quorum::Database& db,
                                         sui::quorum::ConversationEngine& engine,
                                         int64_t conv_id) {
    auto think_id = get_pending_task(db, conv_id, "think");
    mark_done(db, think_id);

    sui::quorum::ParsedOutput parsed;
    parsed.proposals.push_back({"Fix adverse selection", {}, "details..."});
    engine.on_task_complete(think_id, parsed, 0.30);

    return get_pending_task(db, conv_id, "review");
}

// ─── Test 1: start creates conversation ─────────────────────────────────────

static void test_start_creates_conversation() {
    std::cout << "\n=== 1. Start Creates Conversation ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Analyze mm-bot", 5.0, 3);
    check(conv_id > 0, "1: conv_id > 0");

    auto conv = db.get_conversation(conv_id);
    check(conv.has_value(), "1: conversation exists");
    check(conv->state == "thinking", "1: state == thinking (not init)");

    // Verify pending think task
    int64_t task_count = 0;
    db.query(
        "SELECT COUNT(*) FROM tasks WHERE conversation_id = ? "
        "AND task_type = 'think' AND status = 'pending'",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, conv_id);
        },
        [&](sqlite3_stmt* stmt) {
            task_count = sqlite3_column_int64(stmt, 0);
        }
    );
    check(task_count == 1, "1: 1 pending think task");

    // Verify non-empty session_id
    auto think_id = get_pending_task(db, conv_id, "think");
    auto sid = get_session_id(db, think_id);
    check(!sid.empty(), "1: task has non-empty session_id");
}

// ─── Test 2: thinking to reviewing ──────────────────────────────────────────

static void test_thinking_to_reviewing() {
    std::cout << "\n=== 2. Thinking to Reviewing ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test goal", 5.0, 3);
    auto think_id = get_pending_task(db, conv_id, "think");
    mark_done(db, think_id);

    sui::quorum::ParsedOutput parsed;
    parsed.proposals.push_back({"Fix adverse selection", {}, "details..."});
    auto active = engine.on_task_complete(think_id, parsed, 0.30);

    check(active, "2: on_task_complete returns true (still active)");

    auto conv = db.get_conversation(conv_id);
    check(conv->state == "reviewing", "2: state == reviewing");

    auto review_id = get_pending_task(db, conv_id, "review");
    check(review_id > 0, "2: review task created with status pending");
}

// ─── Test 3: reviewing approve to done ──────────────────────────────────────

static void test_reviewing_approve_to_done() {
    std::cout << "\n=== 3. Reviewing Approve to Done ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test goal", 5.0, 3);
    auto review_id = simulate_think_to_review(db, engine, conv_id);
    check(review_id > 0, "3: review task exists");

    mark_done(db, review_id);

    sui::quorum::ParsedOutput parsed;
    parsed.reviews.push_back({"", "approve", "looks good"});
    auto active = engine.on_task_complete(review_id, parsed, 0.20);

    check(!active, "3: on_task_complete returns false (terminal)");

    auto conv = db.get_conversation(conv_id);
    check(conv->state == "done", "3: state == done");
}

// ─── Test 4: reviewing revise loops back ────────────────────────────────────

static void test_reviewing_revise_loops_back() {
    std::cout << "\n=== 4. Reviewing Revise Loops Back ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test goal", 5.0, 3);
    auto review_id = simulate_think_to_review(db, engine, conv_id);
    mark_done(db, review_id);

    sui::quorum::ParsedOutput parsed;
    parsed.reviews.push_back({"", "revise", "needs more data"});
    auto active = engine.on_task_complete(review_id, parsed, 0.20);

    check(active, "4: on_task_complete returns true (still active)");

    auto conv = db.get_conversation(conv_id);
    check(conv->state == "thinking", "4: state == thinking (looped back)");
    check(conv->round == 1, "4: round incremented to 1");

    auto new_think_id = get_pending_task(db, conv_id, "think");
    check(new_think_id > 0, "4: new think task created");
}

// ─── Test 5: revise at max rounds closes ────────────────────────────────────

static void test_revise_at_max_rounds_closes() {
    std::cout << "\n=== 5. Revise at Max Rounds Closes ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    // max_rounds = 1: first revise should fail since 0+1 < 1 is false
    auto conv_id = engine.start("Test goal", 5.0, 1);
    auto review_id = simulate_think_to_review(db, engine, conv_id);
    mark_done(db, review_id);

    sui::quorum::ParsedOutput parsed;
    parsed.reviews.push_back({"", "revise", "needs work"});
    auto active = engine.on_task_complete(review_id, parsed, 0.20);

    check(!active, "5: on_task_complete returns false (terminal)");

    auto conv = db.get_conversation(conv_id);
    check(conv->state == "closed", "5: state == closed (max rounds exhausted)");
}

// ─── Test 6: budget exceeded pauses ─────────────────────────────────────────

static void test_budget_exceeded_pauses() {
    std::cout << "\n=== 6. Budget Exceeded Pauses ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test goal", 0.50, 3);
    auto think_id = get_pending_task(db, conv_id, "think");
    mark_done(db, think_id);

    sui::quorum::ParsedOutput parsed;
    parsed.proposals.push_back({"Some proposal", {}, "content"});
    auto active = engine.on_task_complete(think_id, parsed, 0.60);

    check(!active, "6: on_task_complete returns false (paused)");

    auto conv = db.get_conversation(conv_id);
    check(conv->state == "paused", "6: state == paused");
}

// ─── Test 7: session_id reuse across revisions ──────────────────────────────

static void test_session_id_reuse_across_revisions() {
    std::cout << "\n=== 7. Session ID Reuse Across Revisions ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test goal", 5.0, 3);

    // Get original think task session_id
    auto orig_think_id = get_pending_task(db, conv_id, "think");
    auto orig_sid = get_session_id(db, orig_think_id);

    // think → review → revise → back to thinking
    auto review_id = simulate_think_to_review(db, engine, conv_id);
    mark_done(db, review_id);

    sui::quorum::ParsedOutput parsed;
    parsed.reviews.push_back({"", "revise", "needs more data"});
    engine.on_task_complete(review_id, parsed, 0.20);

    // Get new think task session_id
    auto new_think_id = get_pending_task(db, conv_id, "think");
    auto new_sid = get_session_id(db, new_think_id);

    check(!orig_sid.empty(), "7: original session_id non-empty");
    check(orig_sid == new_sid, "7: session_ids match across revision");
}

// ─── Test 8: reviewer gets different session_id ─────────────────────────────

static void test_reviewer_gets_different_session_id() {
    std::cout << "\n=== 8. Reviewer Gets Different Session ID ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test goal", 5.0, 3);

    auto think_id = get_pending_task(db, conv_id, "think");
    auto think_sid = get_session_id(db, think_id);

    auto review_id = simulate_think_to_review(db, engine, conv_id);
    auto review_sid = get_session_id(db, review_id);

    check(!think_sid.empty(), "8: think session_id non-empty");
    check(!review_sid.empty(), "8: review session_id non-empty");
    check(think_sid != review_sid, "8: thinker and reviewer have different session_ids");
}

// ─── Test 9: no proposals closes ────────────────────────────────────────────

static void test_no_proposals_closes() {
    std::cout << "\n=== 9. No Proposals Closes ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test goal", 5.0, 3);
    auto think_id = get_pending_task(db, conv_id, "think");
    mark_done(db, think_id);

    sui::quorum::ParsedOutput parsed;  // empty — no proposals
    auto active = engine.on_task_complete(think_id, parsed, 0.10);

    check(!active, "9: on_task_complete returns false (closed)");

    auto conv = db.get_conversation(conv_id);
    check(conv->state == "closed", "9: state == closed (no proposals)");
}

// ─── Test 10: close by operator ─────────────────────────────────────────────

static void test_close_by_operator() {
    std::cout << "\n=== 10. Close by Operator ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test goal", 5.0, 3);
    engine.close(conv_id);

    auto conv = db.get_conversation(conv_id);
    check(conv->state == "closed", "10: state == closed by operator");
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== ConversationEngine Tests ===\n";

    test_start_creates_conversation();
    test_thinking_to_reviewing();
    test_reviewing_approve_to_done();
    test_reviewing_revise_loops_back();
    test_revise_at_max_rounds_closes();
    test_budget_exceeded_pauses();
    test_session_id_reuse_across_revisions();
    test_reviewer_gets_different_session_id();
    test_no_proposals_closes();
    test_close_by_operator();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
