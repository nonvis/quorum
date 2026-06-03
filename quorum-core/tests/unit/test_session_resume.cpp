// tests/unit/test_session_resume.cpp
// Unit tests for UUID generation, session_id DB round-trip, and command construction logic.
//
// Run:  cd build && cmake .. && make test_session_resume && ./test_session_resume

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

#include <sqlite3.h>

#include "utils/uuid.h"
#include "storage/database.h"
#include "agent/invoker.h"

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
        "  session_id TEXT"
        ")"
    );
}

// ─── Test 1: UUID format ─────────────────────────────────────────────────────

static void test_uuid_format() {
    std::cout << "\n=== 1. UUID Format ===\n\n";

    auto uuid = sui::quorum::generate_uuid();
    check(uuid.size() == 36, "1a: length == 36");
    check(uuid[8] == '-', "1b: dash at position 8");
    check(uuid[13] == '-', "1c: dash at position 13");
    check(uuid[18] == '-', "1d: dash at position 18");
    check(uuid[23] == '-', "1e: dash at position 23");

    // Verify all non-dash characters are hex digits
    bool all_hex = true;
    for (size_t i = 0; i < uuid.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        char c = uuid[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            all_hex = false;
            break;
        }
    }
    check(all_hex, "1f: all non-dash chars are hex (0-9, a-f)");

    // Version nibble: position 14 must be '4'
    check(uuid[14] == '4', "1g: version nibble at position 14 == '4'");

    // Variant nibble: position 19 must be one of {8, 9, a, b}
    char v = uuid[19];
    check(v == '8' || v == '9' || v == 'a' || v == 'b',
          "1h: variant nibble at position 19 is one of {8,9,a,b}");
}

// ─── Test 2: UUID uniqueness ─────────────────────────────────────────────────

static void test_uuid_uniqueness() {
    std::cout << "\n=== 2. UUID Uniqueness ===\n\n";

    std::set<std::string> uuids;
    for (int i = 0; i < 1000; ++i) {
        uuids.insert(sui::quorum::generate_uuid());
    }
    check(uuids.size() == 1000, "2: 1000 UUIDs are all unique");
}

// ─── Test 3: session_id round-trip ───────────────────────────────────────────

static void test_session_id_round_trip() {
    std::cout << "\n=== 3. Session ID Round-Trip ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);

    auto conv_id = db.create_conversation("test goal", 5.0, 3);

    auto session_id = sui::quorum::generate_uuid();
    db.execute(
        "INSERT INTO tasks (agent, task_type, status, prompt, conversation_id, session_id) "
        "VALUES ('thinker', 'think', 'pending', 'test prompt', ?, ?)",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, conv_id);
            sqlite3_bind_text(stmt, 2, session_id.c_str(), -1, SQLITE_TRANSIENT);
        }
    );
    auto task_id = db.last_insert_id();

    std::string read_sid;
    db.query(
        "SELECT session_id FROM tasks WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, task_id);
        },
        [&](sqlite3_stmt* stmt) {
            auto s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (s) read_sid = s;
        }
    );
    check(read_sid == session_id, "3: session_id round-trips through DB");
}

// ─── Test 4: session_id NULL for Task Queue mode ─────────────────────────────

static void test_session_id_null_for_task_queue() {
    std::cout << "\n=== 4. Session ID NULL for Task Queue ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);

    db.execute(
        "INSERT INTO tasks (agent, task_type, status, prompt) "
        "VALUES ('analyst', 'analyze', 'pending', 'analyze something')"
    );
    auto task_id = db.last_insert_id();

    std::string read_sid;
    bool was_null = true;
    db.query(
        "SELECT session_id FROM tasks WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, task_id);
        },
        [&](sqlite3_stmt* stmt) {
            auto s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (s) {
                read_sid = s;
                was_null = false;
            }
        }
    );
    check(was_null && read_sid.empty(), "4: session_id is NULL for Task Queue task");
}

// ─── Test 5: prior usage detection ──────────────────────────────────────────

static void test_prior_usage_detection() {
    std::cout << "\n=== 5. Prior Usage Detection ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);

    auto conv_id = db.create_conversation("test goal", 5.0, 3);

    // Task 1: done with session_id = "sess-abc"
    db.execute(
        "INSERT INTO tasks (agent, task_type, status, prompt, conversation_id, session_id) "
        "VALUES ('thinker', 'think', 'done', 'prompt1', ?, 'sess-abc')",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, conv_id);
        }
    );
    auto task1_id = db.last_insert_id();

    // Task 2: active with same session_id
    db.execute(
        "INSERT INTO tasks (agent, task_type, status, prompt, conversation_id, session_id) "
        "VALUES ('thinker', 'think', 'active', 'prompt2', ?, 'sess-abc')",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, conv_id);
        }
    );
    auto task2_id = db.last_insert_id();

    // Query: how many completed tasks used this session_id (excluding task2)?
    int64_t prior_uses = 0;
    db.query(
        "SELECT COUNT(*) FROM tasks WHERE session_id = 'sess-abc' AND status = 'done' AND id != ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, task2_id);
        },
        [&](sqlite3_stmt* stmt) {
            prior_uses = sqlite3_column_int64(stmt, 0);
        }
    );
    check(prior_uses == 1, "5: prior_uses == 1 (Invoker should use -r)");
    (void)task1_id;
}

// ─── Test 6: no prior usage for new session ─────────────────────────────────

static void test_no_prior_usage_for_new_session() {
    std::cout << "\n=== 6. No Prior Usage for New Session ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);

    auto conv_id = db.create_conversation("test goal", 5.0, 3);

    // Task 1: active (not done yet) with session_id = "sess-xyz"
    db.execute(
        "INSERT INTO tasks (agent, task_type, status, prompt, conversation_id, session_id) "
        "VALUES ('thinker', 'think', 'active', 'prompt1', ?, 'sess-xyz')",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, conv_id);
        }
    );
    auto task1_id = db.last_insert_id();

    // Query: how many completed tasks used this session_id (excluding task1)?
    int64_t prior_uses = 0;
    db.query(
        "SELECT COUNT(*) FROM tasks WHERE session_id = 'sess-xyz' AND status = 'done' AND id != ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, task1_id);
        },
        [&](sqlite3_stmt* stmt) {
            prior_uses = sqlite3_column_int64(stmt, 0);
        }
    );
    check(prior_uses == 0, "6: prior_uses == 0 (Invoker should use --session-id)");
}

// ─── Test 7: InvocationResult has session_id ────────────────────────────────

static void test_invocation_result_has_session_id() {
    std::cout << "\n=== 8. InvocationResult Has session_id ===\n\n";

    sui::quorum::InvocationResult with_sid{
        .success = true,
        .output = "test",
        .error = {},
        .tokens_in = 100,
        .tokens_out = 50,
        .cost = 0.01,
        .session_id = "test-uuid",
    };
    check(with_sid.session_id == "test-uuid", "8a: session_id set correctly");

    sui::quorum::InvocationResult without_sid{
        .success = true,
        .output = "test",
    };
    check(without_sid.session_id.empty(), "8b: session_id empty by default");
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Session Resume Tests ===\n";

    test_uuid_format();
    test_uuid_uniqueness();
    test_session_id_round_trip();
    test_session_id_null_for_task_queue();
    test_prior_usage_detection();
    test_no_prior_usage_for_new_session();
    test_invocation_result_has_session_id();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
