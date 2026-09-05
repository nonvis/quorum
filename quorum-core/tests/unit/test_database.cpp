// tests/unit/test_database.cpp
// Unit tests for Database conversation CRUD methods.
//
// Run:  cd build && cmake .. && make test_database && ./test_database

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "storage/database.h"
#include "storage/schema.h"

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

    auto id = db.create_conversation("Analyze mm-bot", 3);
    check(id > 0, "1: returned id > 0");

    auto rec = db.get_conversation(id);
    check(rec.has_value(), "1: conversation found");
    check(rec->goal == "Analyze mm-bot", "1: goal matches");
    check(rec->state == "active", "1: state == active");
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

    auto id = db.create_conversation("Test goal", 3);

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

    auto id = db.create_conversation("Budget test", 3);

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

    auto id = db.create_conversation("Pause test", 3);

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

    auto id = db.create_conversation("Complete test", 3);

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

    auto conv_id = db.create_conversation("Link test", 3);

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

// ─── Test 9: Mode column migration (Phase 6 Track 1) ────────────────────────
//
// Synthesizes the pre-Phase-6 schema (no `mode` column), inserts a row,
// runs the column-exists migration that ships with the daemon, and verifies:
//   - existing rows surface as mode = 'generic' (DEFAULT applied by ALTER)
//   - new INSERTs without an explicit mode also default to 'generic'
//   - explicit INSERTs with mode = 'brainstorm' persist that value
//
// Helpers (column_exists / migrate) mirror src/main.cpp init_schema() — kept
// inline so the test exercises the migration logic without pulling main.cpp.

static bool column_exists(sui::quorum::Database& db,
                          const std::string& table,
                          const std::string& column) {
    bool found = false;
    db.query(
        "SELECT 1 FROM pragma_table_info('" + table + "') WHERE name = '" + column + "'",
        [&](sqlite3_stmt*) { found = true; }
    );
    return found;
}

static void init_pre_phase6_conversations_table(sui::quorum::Database& db) {
    // Schema as it was before Phase 6 (no `mode` column)
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
        "  team TEXT"
        ")"
    );
}

static void test_mode_column_migration() {
    std::cout << "\n=== 9. Mode Column Migration ===\n\n";

    sui::quorum::Database db(":memory:");

    // 1. Synthesize pre-Phase-6 DB
    init_pre_phase6_conversations_table(db);
    check(!column_exists(db, "conversations", "mode"),
          "9: pre-migration: mode column absent");

    // Insert a row under the old schema
    auto pre_id = db.create_conversation("legacy goal", 3);
    check(pre_id > 0, "9: pre-migration: legacy row inserted");

    // 2. Run the migrations (matches src/main.cpp init_schema migration).
    //    `no_vault_write` is Phase 10 Track 5 and is referenced by
    //    get_conversation()'s SELECT, so it must be present even when this
    //    test focuses on the mode-column migration.
    if (!column_exists(db, "conversations", "mode")) {
        db.execute("ALTER TABLE conversations ADD COLUMN mode TEXT NOT NULL DEFAULT 'generic'");
    }
    if (!column_exists(db, "conversations", "no_vault_write")) {
        db.execute("ALTER TABLE conversations ADD COLUMN no_vault_write INTEGER NOT NULL DEFAULT 0");
    }
    // Phase 14.1 — gated/gate_cleared are also referenced by
    // get_conversation()'s SELECT, so apply them in this synthetic migration too.
    if (!column_exists(db, "conversations", "gated")) {
        db.execute("ALTER TABLE conversations ADD COLUMN gated INTEGER NOT NULL DEFAULT 0");
    }
    if (!column_exists(db, "conversations", "gate_cleared")) {
        db.execute("ALTER TABLE conversations ADD COLUMN gate_cleared INTEGER NOT NULL DEFAULT 0");
    }
    check(column_exists(db, "conversations", "mode"),
          "9: post-migration: mode column present");
    check(column_exists(db, "conversations", "gated"),
          "9: post-migration: gated column present");
    check(column_exists(db, "conversations", "gate_cleared"),
          "9: post-migration: gate_cleared column present");

    // 3. Existing row should now read mode = 'generic'
    auto legacy = db.get_conversation(pre_id);
    check(legacy.has_value(), "9: legacy row still readable");
    check(legacy->mode == "generic",
          "9: legacy row has mode = 'generic' after ALTER");

    // 4. New INSERT without explicit mode → default 'generic'
    auto new_id = db.create_conversation("post-migration goal", 3);
    auto new_rec = db.get_conversation(new_id);
    check(new_rec.has_value(), "9: new row readable");
    check(new_rec->mode == "generic",
          "9: new row defaults to mode = 'generic'");

    // 5. Explicit INSERT with mode = 'brainstorm' persists
    db.execute(
        "INSERT INTO conversations (goal, mode) VALUES (?, 'brainstorm')",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_text(stmt, 1, "brainstorm goal", -1, SQLITE_TRANSIENT);
        }
    );
    auto brain_id = db.last_insert_id();
    auto brain_rec = db.get_conversation(brain_id);
    check(brain_rec.has_value(), "9: brainstorm row readable");
    check(brain_rec->mode == "brainstorm",
          "9: explicit brainstorm value persists");
}

// ─── Test 10: tasks.summary column migration (A4) ───────────────────────────
//
// Synthesizes the pre-A4 tasks table (no `summary`), runs the guarded
// ALTER twice, and verifies: the column appears, neither run errors, and the
// second run is a no-op (the column list is byte-identical and `summary`
// appears exactly once). PRAGMA table_info is printed for both runs.

static std::vector<std::string> table_columns(sui::quorum::Database& db,
                                              const std::string& table) {
    std::vector<std::string> cols;
    db.query("SELECT name FROM pragma_table_info('" + table + "')",
             [&](sqlite3_stmt* stmt) {
                 auto n = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                 if (n) cols.emplace_back(n);
             });
    return cols;
}

static void init_pre_a4_tasks_table(sui::quorum::Database& db) {
    // tasks as it shipped before A4 (storage/schema.h + the Phase 7 ALTERs)
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
        "  conversation_id INTEGER,"
        "  session_id TEXT,"
        "  system_prompt TEXT,"
        "  cache_creation_input_tokens INTEGER,"
        "  cache_read_input_tokens INTEGER"
        ")"
    );
}

// Mirrors src/main.cpp init_schema()'s summary migration. execute_ok() is used
// (not execute()) so a SQL error is observable rather than only logged.
static bool migrate_tasks_summary(sui::quorum::Database& db) {
    if (column_exists(db, "tasks", "summary")) return true;  // idempotent guard
    return db.execute_ok("ALTER TABLE tasks ADD COLUMN summary TEXT");
}

static void test_tasks_summary_migration() {
    std::cout << "\n=== 10. tasks.summary Column Migration (A4) ===\n\n";

    sui::quorum::Database db(":memory:");
    init_pre_a4_tasks_table(db);

    check(!column_exists(db, "tasks", "summary"),
          "10: pre-migration: tasks.summary absent");

    // A row written under the pre-A4 schema.
    db.execute(
        "INSERT INTO tasks (agent, task_type, status, prompt, result) "
        "VALUES ('leader', 'scan', 'done', 'p', 'legacy result')"
    );
    auto legacy_id = db.last_insert_id();
    check(legacy_id > 0, "10: pre-migration: legacy row inserted");

    // ── Run 1 ────────────────────────────────────────────────────────────────
    check(migrate_tasks_summary(db), "10: first migration run reports success");
    auto after_first = table_columns(db, "tasks");
    std::cout << "    PRAGMA table_info(tasks) after run 1: ";
    for (const auto& c : after_first) std::cout << c << " ";
    std::cout << "\n";

    check(column_exists(db, "tasks", "summary"),
          "10: post-migration: tasks.summary present");

    // ── Run 2 — must be a no-op, not a duplicate-column error ───────────────
    check(migrate_tasks_summary(db), "10: second migration run reports success");
    auto after_second = table_columns(db, "tasks");
    std::cout << "    PRAGMA table_info(tasks) after run 2: ";
    for (const auto& c : after_second) std::cout << c << " ";
    std::cout << "\n";

    check(after_second == after_first,
          "10: second run is a no-op (column list unchanged)");

    int summary_cols = 0;
    for (const auto& c : after_second) if (c == "summary") ++summary_cols;
    check(summary_cols == 1, "10: exactly one summary column after two runs");

    // The legacy row reads NULL — absent, not "".
    auto nulls = db.query_int(
        "SELECT COUNT(*) FROM tasks WHERE id = " + std::to_string(legacy_id) +
        " AND summary IS NULL");
    check(nulls == 1, "10: the pre-A4 row reads summary IS NULL after the ALTER");
}

// ─── Test 11: create_schema() carries every ALTERed column ──────────────────
//
// `quorum init` calls create_schema() and nothing else; only the daemon path
// runs main.cpp init_schema()'s guarded ALTERs. So any column that exists ONLY
// as an ALTER is absent from a DB the CLI created — the A4 doer hit exactly
// that with the two cache-token columns, and the census turned up
// conversations.team as a third (main.cpp print_conversations SELECTs it).
//
// This test pins both halves of the contract:
//   (a) a fresh create_schema() DB already HAS the columns, and
//   (b) running the guarded migration on top is a NO-OP — the column list is
//       byte-identical and no name appears twice.
// migrate_all() below mirrors src/main.cpp init_schema() verbatim (same guard,
// same SQL). execute_ok() surfaces a SQL error as a return value.

static bool add_column_if_missing(sui::quorum::Database& db,
                                  const std::string& table,
                                  const std::string& column,
                                  const std::string& decl) {
    if (column_exists(db, table, column)) return true;  // idempotent guard
    return db.execute_ok("ALTER TABLE " + table + " ADD COLUMN " + decl);
}

// Verbatim mirror of the guarded ALTERs in src/main.cpp init_schema()
// (main.cpp:236-269 — the column_exists() guard is what makes each one
// idempotent when the column is already present).
static bool migrate_all(sui::quorum::Database& db) {
    bool ok = true;
    ok &= add_column_if_missing(db, "tasks", "conversation_id",
        "conversation_id INTEGER REFERENCES conversations(id)");
    ok &= add_column_if_missing(db, "tasks", "session_id", "session_id TEXT");
    ok &= add_column_if_missing(db, "conversations", "current_agent", "current_agent TEXT");
    ok &= add_column_if_missing(db, "conversations", "path_index",
        "path_index INTEGER NOT NULL DEFAULT 0");
    ok &= add_column_if_missing(db, "conversations", "team", "team TEXT");
    ok &= add_column_if_missing(db, "conversations", "mode",
        "mode TEXT NOT NULL DEFAULT 'generic'");
    ok &= add_column_if_missing(db, "conversations", "no_vault_write",
        "no_vault_write INTEGER NOT NULL DEFAULT 0");
    ok &= add_column_if_missing(db, "conversations", "gated",
        "gated INTEGER NOT NULL DEFAULT 0");
    ok &= add_column_if_missing(db, "conversations", "gate_cleared",
        "gate_cleared INTEGER NOT NULL DEFAULT 0");
    ok &= add_column_if_missing(db, "tasks", "system_prompt", "system_prompt TEXT");
    ok &= add_column_if_missing(db, "tasks", "cache_creation_input_tokens",
        "cache_creation_input_tokens INTEGER");
    ok &= add_column_if_missing(db, "tasks", "cache_read_input_tokens",
        "cache_read_input_tokens INTEGER");
    ok &= add_column_if_missing(db, "tasks", "summary", "summary TEXT");
    return ok;
}

static void print_cols(const char* label, const std::vector<std::string>& cols) {
    std::cout << "    " << label << ": ";
    for (const auto& c : cols) std::cout << c << " ";
    std::cout << "\n";
}

static void test_create_schema_covers_every_alter() {
    std::cout << "\n=== 11. create_schema() covers every ALTERed column ===\n\n";

    sui::quorum::Database db(":memory:");
    sui::quorum::create_schema(db);  // exactly what `quorum init` runs

    auto tasks_fresh = table_columns(db, "tasks");
    auto convs_fresh = table_columns(db, "conversations");
    print_cols("PRAGMA table_info(tasks) after create_schema", tasks_fresh);
    print_cols("PRAGMA table_info(conversations) after create_schema", convs_fresh);

    // (a) the three columns that used to be ALTER-only
    check(column_exists(db, "tasks", "cache_creation_input_tokens"),
          "11: create_schema: tasks.cache_creation_input_tokens present");
    check(column_exists(db, "tasks", "cache_read_input_tokens"),
          "11: create_schema: tasks.cache_read_input_tokens present");
    check(column_exists(db, "conversations", "team"),
          "11: create_schema: conversations.team present");

    // (b) the migration on top of a fresh schema changes nothing
    check(migrate_all(db), "11: migration on a fresh schema reports success");

    auto tasks_after = table_columns(db, "tasks");
    auto convs_after = table_columns(db, "conversations");
    print_cols("PRAGMA table_info(tasks) after migration", tasks_after);
    print_cols("PRAGMA table_info(conversations) after migration", convs_after);

    check(tasks_after == tasks_fresh,
          "11: migration is a no-op on tasks (column list unchanged)");
    check(convs_after == convs_fresh,
          "11: migration is a no-op on conversations (column list unchanged)");

    // (No "exactly one <col>" count assertion here. SQLite rejects a duplicate
    // ADD COLUMN outright, so the count can only be 0 or 1, and 0 is already
    // caught above — no mutation can red such a check while its neighbours
    // pass. Test 10 above carries the same idiom; flagged, not touched.)

    // The declared type must match the ALTER's, or a fresh DB and a migrated
    // DB disagree about what the daemon wrote. Both are plain INTEGER with no
    // DEFAULT ⇒ an untouched row reads NULL, not 0, on either.
    db.execute("INSERT INTO tasks (agent, task_type, prompt) "
               "VALUES ('leader', 'scan', 'p')");
    auto nulls = db.query_int(
        "SELECT COUNT(*) FROM tasks WHERE cache_creation_input_tokens IS NULL "
        "AND cache_read_input_tokens IS NULL");
    check(nulls == 1,
          "11: fresh-schema row reads both cache columns as NULL (no DEFAULT, "
          "matching the ALTER)");
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
    test_mode_column_migration();
    test_tasks_summary_migration();
    test_create_schema_covers_every_alter();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
