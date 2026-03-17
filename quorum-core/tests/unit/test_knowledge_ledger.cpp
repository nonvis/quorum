// tests/unit/test_knowledge_ledger.cpp
// Unit tests for knowledge_ledger Database methods.
//
// Run:  cd build && cmake .. && make test_knowledge_ledger && ./test_knowledge_ledger

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
        "  paused_reason TEXT"
        ")"
    );
}

static void init_knowledge_ledger_table(sui::quorum::Database& db) {
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
}

// ─── Test I: Append and retrieve ─────────────────────────────────────────────

static void test_append_and_retrieve() {
    std::cout << "\n=== I. Append and Retrieve ===\n\n";

    sui::quorum::Database db(":memory:");
    init_conversations_table(db);
    init_knowledge_ledger_table(db);

    auto conv_id = db.create_conversation("Test cycle", 5.0, 3);

    auto id1 = db.append_knowledge(conv_id, "thinker", 1, "arch", "Decided on X");
    check(id1 > 0, "I: first insert returned id > 0");

    auto id2 = db.append_knowledge(conv_id, "doer", 2, "impl", "Built Y");
    check(id2 > id1, "I: second insert id > first");

    auto id3 = db.append_knowledge(conv_id, "doer", 2, "", "Observation without topic");
    check(id3 > id2, "I: third insert id > second");

    auto text = db.get_cycle_knowledge(conv_id);
    check(text.find("Turn 1") != std::string::npos, "I: text contains Turn 1");
    check(text.find("Turn 2") != std::string::npos, "I: text contains Turn 2");
    check(text.find("Decided on X") != std::string::npos, "I: text contains 'Decided on X'");
    check(text.find("Built Y") != std::string::npos, "I: text contains 'Built Y'");
    check(text.find("Observation without topic") != std::string::npos,
          "I: text contains topicless observation");

    auto count = db.count_cycle_knowledge(conv_id);
    check(count == 3, "I: count == 3");
}

// ─── Test J: Empty cycle ─────────────────────────────────────────────────────

static void test_empty_cycle() {
    std::cout << "\n=== J. Empty Cycle ===\n\n";

    sui::quorum::Database db(":memory:");
    init_conversations_table(db);
    init_knowledge_ledger_table(db);

    auto conv_id = db.create_conversation("Empty cycle", 5.0, 3);

    auto text = db.get_cycle_knowledge(conv_id);
    check(text.empty(), "J: get_cycle_knowledge returns empty string");

    auto count = db.count_cycle_knowledge(conv_id);
    check(count == 0, "J: count == 0");
}

// ─── Test K: Cross-cycle isolation ───────────────────────────────────────────

static void test_cross_cycle_isolation() {
    std::cout << "\n=== K. Cross-cycle Isolation ===\n\n";

    sui::quorum::Database db(":memory:");
    init_conversations_table(db);
    init_knowledge_ledger_table(db);

    auto cycle1 = db.create_conversation("Cycle 1", 5.0, 3);
    auto cycle2 = db.create_conversation("Cycle 2", 5.0, 3);

    db.append_knowledge(cycle1, "thinker", 1, "arch", "Cycle 1 decision");
    db.append_knowledge(cycle2, "doer", 1, "impl", "Cycle 2 work");

    auto text1 = db.get_cycle_knowledge(cycle1);
    check(text1.find("Cycle 1 decision") != std::string::npos,
          "K: cycle 1 contains its own data");
    check(text1.find("Cycle 2 work") == std::string::npos,
          "K: cycle 1 does NOT contain cycle 2 data");

    auto count1 = db.count_cycle_knowledge(cycle1);
    check(count1 == 1, "K: cycle 1 count == 1");

    auto count2 = db.count_cycle_knowledge(cycle2);
    check(count2 == 1, "K: cycle 2 count == 1");
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Knowledge Ledger Tests ===\n";

    test_append_and_retrieve();
    test_empty_cycle();
    test_cross_cycle_isolation();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
