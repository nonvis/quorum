// tests/unit/test_phase_plan_checkoff.cpp
// Unit tests for the daemon-side phase-plan checkoff backstop.
//
// Run:  cd build && cmake .. && make test_phase_plan_checkoff && ./test_phase_plan_checkoff

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <unistd.h>
#include <sqlite3.h>

#include "storage/database.h"
#include "daemon/phase_plan_checkoff.h"

namespace fs = std::filesystem;

// ── helpers ──────────────────────────────────────────────────────────────────

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
        "  session_id TEXT"
        ")"
    );
}

static int64_t insert_conversation(sui::quorum::Database& db, const std::string& goal) {
    db.execute(
        "INSERT INTO conversations (goal) VALUES (?)",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_text(stmt, 1, goal.c_str(), -1, SQLITE_TRANSIENT);
        }
    );
    return db.last_insert_id();
}

static void insert_task(sui::quorum::Database& db, int64_t conv_id,
                        const std::string& agent, const std::string& prompt) {
    db.execute(
        "INSERT INTO tasks (agent, task_type, prompt, conversation_id) "
        "VALUES (?, 'turn', ?, ?)",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_text(stmt, 1, agent.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, prompt.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 3, conv_id);
        }
    );
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

static void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::trunc);
    f << content;
}

// Make a fresh tmp dir per test so they can't collide.
static fs::path make_tmp_dir(const std::string& tag) {
    auto base = fs::temp_directory_path() /
        ("quorum_phase_plan_test_" + tag + "_" + std::to_string(::getpid()));
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

// ── Test A: basic match + flip ────────────────────────────────────────────────
static void test_A_basic_flip() {
    std::cout << "\n=== A. basic match + flip ===\n\n";

    auto tdir = make_tmp_dir("A");
    auto plan = tdir / "phase-1-plan.md";
    write_file(plan,
        "# Phase 1\n"
        "\n"
        "- [ ] Task 4: foo\n"
        "- [ ] Task 5: bar\n"
    );
    write_file(tdir / ".quorum" / "current_phase.md", plan.string() + "\n");

    sui::quorum::Database db(":memory:");
    init_schema(db);
    auto conv = insert_conversation(db, "Goal A");
    insert_task(db, conv, "leader", "Task 4: implement foo\n\nDo the thing.");
    insert_task(db, conv, "doer", "Task 4: implement foo\n\nDo the thing.");

    int n = sui::quorum::checkoff_completed_tasks(db, conv, tdir.string(), "2026-05-09");
    check(n == 1, "A: one line updated");

    auto contents = read_file(plan);
    check(contents.find("- [x] Task 4: foo (2026-05-09)") != std::string::npos,
          "A: Task 4 line is checked with date");
    check(contents.find("- [ ] Task 5: bar") != std::string::npos,
          "A: Task 5 (untouched) still unchecked");
}

// ── Test B: idempotency ───────────────────────────────────────────────────────
static void test_B_idempotency() {
    std::cout << "\n=== B. idempotency ===\n\n";

    auto tdir = make_tmp_dir("B");
    auto plan = tdir / "phase-1-plan.md";
    write_file(plan,
        "- [ ] Task 1: alpha\n"
        "- [ ] Task 2: beta\n"
    );
    write_file(tdir / ".quorum" / "current_phase.md", plan.string() + "\n");

    sui::quorum::Database db(":memory:");
    init_schema(db);
    auto conv = insert_conversation(db, "Goal B");
    insert_task(db, conv, "leader", "Task 1: alpha");

    int n1 = sui::quorum::checkoff_completed_tasks(db, conv, tdir.string(), "2026-05-09");
    check(n1 == 1, "B: first run flips Task 1");

    int n2 = sui::quorum::checkoff_completed_tasks(db, conv, tdir.string(), "2026-05-09");
    check(n2 == 0, "B: second run is a no-op");

    auto contents = read_file(plan);
    // Count occurrences of "(2026-05-09)" — must be exactly 1.
    size_t count = 0;
    size_t pos = 0;
    while ((pos = contents.find("(2026-05-09)", pos)) != std::string::npos) {
        ++count;
        pos += 12;
    }
    check(count == 1, "B: date appears exactly once (no double-suffix)");
}

// ── Test C: missing pointer file ─────────────────────────────────────────────
static void test_C_missing_pointer() {
    std::cout << "\n=== C. missing pointer file ===\n\n";

    auto tdir = make_tmp_dir("C");
    // No .quorum/ directory at all.

    sui::quorum::Database db(":memory:");
    init_schema(db);
    auto conv = insert_conversation(db, "Goal C");
    insert_task(db, conv, "leader", "Task 3: anything");

    int n = sui::quorum::checkoff_completed_tasks(db, conv, tdir.string(), "2026-05-09");
    check(n == 0, "C: returns 0 when current_phase.md is missing");
}

// ── Test D: #N form ──────────────────────────────────────────────────────────
static void test_D_hash_form() {
    std::cout << "\n=== D. #N form ===\n\n";

    auto tdir = make_tmp_dir("D");
    auto plan = tdir / "plan.md";
    write_file(plan,
        "- [ ] #7 Bar\n"
        "- [ ] #8 Baz\n"
    );
    write_file(tdir / ".quorum" / "current_phase.md", plan.string() + "\n");

    sui::quorum::Database db(":memory:");
    init_schema(db);
    auto conv = insert_conversation(db, "Goal D");
    insert_task(db, conv, "leader", "Task 7: do bar");

    int n = sui::quorum::checkoff_completed_tasks(db, conv, tdir.string(), "2026-05-09");
    check(n == 1, "D: #7 line flipped");

    auto contents = read_file(plan);
    check(contents.find("- [x] #7 Bar (2026-05-09)") != std::string::npos,
          "D: #7 line shows checked + date");
    check(contents.find("- [ ] #8 Baz") != std::string::npos,
          "D: #8 untouched");
}

// ── Test E: no matching task in plan ─────────────────────────────────────────
static void test_E_no_match() {
    std::cout << "\n=== E. no matching task ===\n\n";

    auto tdir = make_tmp_dir("E");
    auto plan = tdir / "plan.md";
    auto original =
        std::string("- [ ] Task 1: alpha\n")
        + "- [ ] Task 2: beta\n";
    write_file(plan, original);
    write_file(tdir / ".quorum" / "current_phase.md", plan.string() + "\n");

    sui::quorum::Database db(":memory:");
    init_schema(db);
    auto conv = insert_conversation(db, "Goal E");
    insert_task(db, conv, "leader", "Task 99: not in plan");

    int n = sui::quorum::checkoff_completed_tasks(db, conv, tdir.string(), "2026-05-09");
    check(n == 0, "E: returns 0 when no matching task in plan");
    check(read_file(plan) == original, "E: plan file unchanged");
}

// ── main ─────────────────────────────────────────────────────────────────────
int main() {
    std::cout << "=== Phase Plan Checkoff Unit Tests ===\n";

    test_A_basic_flip();
    test_B_idempotency();
    test_C_missing_pointer();
    test_D_hash_form();
    test_E_no_match();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
