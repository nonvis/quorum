// tests/unit/test_database.cpp
// Unit tests for Database conversation CRUD methods.
//
// Run:  cd build && cmake .. && make test_database && ./test_database

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include <sqlite3.h>

#include "storage/database.h"

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

// Schema helpers — duplicated here so test_pipeline.cpp is unmodified.
static void init_conversations_table(sui::quorum::Database& db) {
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
}

static void init_tasks_table(sui::quorum::Database& db) {
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

// ─── Test 1: Create conversation with defaults ──────────────────────────────

static void test_create_conversation_defaults() {
    std::cout << "\n=== 1. Create Conversation Defaults ===\n\n";

    sui::quorum::Database db(":memory:");
    init_conversations_table(db);
    init_tasks_table(db);

    auto id = db.create_conversation("Analyze mm-bot", 5.0, 3);
    check(id > 0, "1: returned id > 0");

    auto rec = db.get_conversation(id);
    check(rec.has_value(), "1: conversation found");
    check(rec->goal == "Analyze mm-bot", "1: goal matches");
    check(rec->state == "init", "1: state == init");
    check(rec->round == 0, "1: round == 0");
    check(rec->max_rounds == 3, "1: max_rounds == 3");
    check(std::abs(rec->budget_usd - 5.0) < 0.01, "1: budget_usd ~= 5.0");
    check(std::abs(rec->spent_usd - 0.0) < 0.01, "1: spent_usd ~= 0.0");
}

// ─── Test 2: Update state ───────────────────────────────────────────────────

static void test_update_state() {
    std::cout << "\n=== 2. Update State ===\n\n";

    sui::quorum::Database db(":memory:");
    init_conversations_table(db);

    auto id = db.create_conversation("Test goal", 5.0, 3);

    db.update_conversation_state(id, "thinking");
    auto rec = db.get_conversation(id);
    check(rec->state == "thinking", "2: state == thinking");

    db.update_conversation_state(id, "reviewing");
    rec = db.get_conversation(id);
    check(rec->state == "reviewing", "2: state == reviewing");
}

// ─── Test 3: Spent accumulation ─────────────────────────────────────────────

static void test_update_spent_accumulation() {
    std::cout << "\n=== 3. Spent Accumulation ===\n\n";

    sui::quorum::Database db(":memory:");
    init_conversations_table(db);

    auto id = db.create_conversation("Budget test", 5.0, 3);

    db.update_conversation_spent(id, 0.50);
    auto rec = db.get_conversation(id);
    check(std::abs(rec->spent_usd - 0.50) < 0.01, "3: spent_usd ~= 0.50 after first add");

    db.update_conversation_spent(id, 1.20);
    rec = db.get_conversation(id);
    check(std::abs(rec->spent_usd - 1.70) < 0.01, "3: spent_usd ~= 1.70 (accumulated)");
}

// ─── Test 4: Pause with reason ──────────────────────────────────────────────

static void test_pause_with_reason() {
    std::cout << "\n=== 4. Pause With Reason ===\n\n";

    sui::quorum::Database db(":memory:");
    init_conversations_table(db);

    auto id = db.create_conversation("Pause test", 5.0, 3);

    db.pause_conversation(id, "budget exceeded");

    auto rec = db.get_conversation(id);
    check(rec->state == "paused", "4: state == paused");

    // Verify paused_reason via direct query (not in ConversationRecord)
    std::string reason;
    db.query(
        "SELECT paused_reason FROM conversations WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, id);
        },
        [&](sqlite3_stmt* stmt) {
            auto r = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (r) reason = r;
        }
    );
    check(reason == "budget exceeded", "4: paused_reason == 'budget exceeded'");
}

// ─── Test 5: Complete conversation ──────────────────────────────────────────

static void test_complete_conversation() {
    std::cout << "\n=== 5. Complete Conversation ===\n\n";

    sui::quorum::Database db(":memory:");
    init_conversations_table(db);

    auto id = db.create_conversation("Complete test", 5.0, 3);

    db.complete_conversation(id);

    auto rec = db.get_conversation(id);
    check(rec->state == "done", "5: state == done");

    // Verify completed_at IS NOT NULL
    bool has_completed_at = false;
    db.query(
        "SELECT completed_at FROM conversations WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, id);
        },
        [&](sqlite3_stmt* stmt) {
            has_completed_at = (sqlite3_column_type(stmt, 0) != SQLITE_NULL);
        }
    );
    check(has_completed_at, "5: completed_at IS NOT NULL");
}

// ─── Test 6: Task-conversation link ─────────────────────────────────────────

static void test_task_conversation_link() {
    std::cout << "\n=== 6. Task-Conversation Link ===\n\n";

    sui::quorum::Database db(":memory:");
    init_conversations_table(db);
    init_tasks_table(db);

    auto conv_id = db.create_conversation("Link test", 5.0, 3);

    // Insert a task with conversation_id set
    db.execute(
        "INSERT INTO tasks (agent, task_type, prompt, conversation_id, session_id) "
        "VALUES ('thinker', 'think', 'test prompt', ?, 'sess-abc')",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, conv_id);
        }
    );
    auto task_id = db.last_insert_id();

    auto result = db.get_conversation_for_task(task_id);
    check(result.has_value(), "6: get_conversation_for_task returned a value");
    check(*result == conv_id, "6: returned conv_id matches");
}

// ─── Test 7: Task without conversation ──────────────────────────────────────

static void test_task_without_conversation() {
    std::cout << "\n=== 7. Task Without Conversation ===\n\n";

    sui::quorum::Database db(":memory:");
    init_conversations_table(db);
    init_tasks_table(db);

    // Insert a task WITHOUT conversation_id (NULL)
    db.execute(
        "INSERT INTO tasks (agent, task_type, prompt) "
        "VALUES ('thinker', 'think', 'no conversation')"
    );
    auto task_id = db.last_insert_id();

    auto result = db.get_conversation_for_task(task_id);
    check(!result.has_value(), "7: returns nullopt for task without conversation_id");
}

// ─── Test 8: Get nonexistent conversation ───────────────────────────────────

static void test_get_nonexistent_conversation() {
    std::cout << "\n=== 8. Get Nonexistent Conversation ===\n\n";

    sui::quorum::Database db(":memory:");
    init_conversations_table(db);

    auto result = db.get_conversation(9999);
    check(!result.has_value(), "8: returns nullopt for nonexistent conversation");
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Database Conversation CRUD Tests ===\n";

    test_create_conversation_defaults();
    test_update_state();
    test_update_spent_accumulation();
    test_pause_with_reason();
    test_complete_conversation();
    test_task_conversation_link();
    test_task_without_conversation();
    test_get_nonexistent_conversation();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
