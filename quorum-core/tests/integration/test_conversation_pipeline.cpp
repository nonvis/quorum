// tests/integration/test_conversation_pipeline.cpp
// Integration test for the Conversation Mode pipeline — exercises the full
// INIT -> THINKING -> REVIEWING -> DONE state machine end-to-end without
// spawning any claude -p processes.
//
// Run:  cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure

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

// Simulate what the daemon dispatch loop does: mark task done, then call engine
static void simulate_task_complete(
    sui::quorum::Database& db,
    sui::quorum::ConversationEngine& engine,
    int64_t task_id,
    const sui::quorum::ParsedOutput& parsed,
    double cost)
{
    db.execute(
        "UPDATE tasks SET status = 'done', cost = ? WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_double(stmt, 1, cost);
            sqlite3_bind_int64(stmt, 2, task_id);
        }
    );
    engine.on_task_complete(task_id, parsed, cost);
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

static std::string get_state(sui::quorum::Database& db, int64_t conv_id) {
    auto conv = db.get_conversation(conv_id);
    return conv ? conv->state : "";
}

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

static std::string get_task_agent(sui::quorum::Database& db, int64_t task_id) {
    std::string agent;
    db.query(
        "SELECT agent FROM tasks WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, task_id);
        },
        [&](sqlite3_stmt* stmt) {
            auto s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (s) agent = s;
        }
    );
    return agent;
}

static int64_t get_task_count(sui::quorum::Database& db, int64_t conv_id) {
    return db.query_int(
        "SELECT COUNT(*) FROM tasks WHERE conversation_id = "
        + std::to_string(conv_id));
}

// ─── Test A: Happy Path (INIT -> THINKING -> REVIEWING -> DONE) ─────────────

static void test_happy_path() {
    std::cout << "\n=== A. Happy Path ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Optimize spread parameters", 5.0, 3);
    check(get_state(db, conv_id) == "thinking", "A: state = thinking after start");

    auto think_id = get_pending_task(db, conv_id, "think");
    check(get_task_agent(db, think_id) == "thinker", "A: think task agent = thinker");
    check(!get_session_id(db, think_id).empty(), "A: think task has session_id");

    sui::quorum::ParsedOutput think_out;
    think_out.proposals.push_back({"Tighten Spread", {}, "reduce 8->5 bps"});
    simulate_task_complete(db, engine, think_id, think_out, 0.30);
    check(get_state(db, conv_id) == "reviewing", "A: state = reviewing after think");

    auto review_id = get_pending_task(db, conv_id, "review");
    check(get_task_agent(db, review_id) == "reviewer", "A: review task agent = reviewer");

    sui::quorum::ParsedOutput review_out;
    review_out.reviews.push_back({"", "approve", "LGTM"});
    simulate_task_complete(db, engine, review_id, review_out, 0.25);
    check(get_state(db, conv_id) == "done", "A: state = done after approve");

    auto conv = db.get_conversation(conv_id);
    check(std::abs(conv->spent_usd - 0.55) < 0.01, "A: spent_usd ~= 0.55");
}

// ─── Test B: REVISE Cycle with Session ID Reuse ─────────────────────────────

static void test_revise_with_session_reuse() {
    std::cout << "\n=== B. REVISE Cycle with Session ID Reuse ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Analyze performance", 5.0, 3);
    auto orig_think_id = get_pending_task(db, conv_id, "think");
    auto orig_sid = get_session_id(db, orig_think_id);

    // Think -> review with PROPOSAL
    sui::quorum::ParsedOutput think_out;
    think_out.proposals.push_back({"Performance Fix", {}, "details"});
    simulate_task_complete(db, engine, orig_think_id, think_out, 0.20);

    // Review -> revise
    auto review_id1 = get_pending_task(db, conv_id, "review");
    sui::quorum::ParsedOutput revise_out;
    revise_out.reviews.push_back({"", "revise", "Need more data"});
    simulate_task_complete(db, engine, review_id1, revise_out, 0.15);

    check(get_state(db, conv_id) == "thinking", "B: state = thinking after revise");
    auto conv = db.get_conversation(conv_id);
    check(conv->round == 1, "B: round = 1 after revise");

    auto new_think_id = get_pending_task(db, conv_id, "think");
    auto new_sid = get_session_id(db, new_think_id);
    check(!orig_sid.empty(), "B: original session_id non-empty");
    check(orig_sid == new_sid, "B: session_id reused across revise");

    // Second think -> approve
    sui::quorum::ParsedOutput think_out2;
    think_out2.proposals.push_back({"Updated Fix", {}, "more details"});
    simulate_task_complete(db, engine, new_think_id, think_out2, 0.20);

    auto review_id2 = get_pending_task(db, conv_id, "review");
    sui::quorum::ParsedOutput approve_out;
    approve_out.reviews.push_back({"", "approve", "Looks good now"});
    simulate_task_complete(db, engine, review_id2, approve_out, 0.15);

    check(get_state(db, conv_id) == "done", "B: state = done after approval");

    conv = db.get_conversation(conv_id);
    check(std::abs(conv->spent_usd - 0.70) < 0.01, "B: spent_usd ~= 0.70");
}

// ─── Test C: Max Rounds Exhausted -> CLOSED ─────────────────────────────────

static void test_max_rounds_exhausted() {
    std::cout << "\n=== C. Max Rounds Exhausted -> CLOSED ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test max rounds", 10.0, 2);

    // Round 0: think -> review -> revise
    auto think_id1 = get_pending_task(db, conv_id, "think");
    sui::quorum::ParsedOutput think_out1;
    think_out1.proposals.push_back({"Proposal R0", {}, "content"});
    simulate_task_complete(db, engine, think_id1, think_out1, 0.10);

    auto review_id1 = get_pending_task(db, conv_id, "review");
    sui::quorum::ParsedOutput revise_out1;
    revise_out1.reviews.push_back({"", "revise", "not good enough"});
    simulate_task_complete(db, engine, review_id1, revise_out1, 0.10);

    auto conv = db.get_conversation(conv_id);
    check(conv->round == 1, "C: round = 1 after first revise");
    check(get_state(db, conv_id) == "thinking", "C: state = thinking after first revise");

    // Round 1: think -> review -> revise (should close: 1+1=2 >= max_rounds=2)
    auto think_id2 = get_pending_task(db, conv_id, "think");
    sui::quorum::ParsedOutput think_out2;
    think_out2.proposals.push_back({"Proposal R1", {}, "updated"});
    simulate_task_complete(db, engine, think_id2, think_out2, 0.10);

    auto review_id2 = get_pending_task(db, conv_id, "review");
    sui::quorum::ParsedOutput revise_out2;
    revise_out2.reviews.push_back({"", "revise", "still not good"});
    simulate_task_complete(db, engine, review_id2, revise_out2, 0.10);

    check(get_state(db, conv_id) == "closed", "C: state = closed (max rounds exhausted)");
    conv = db.get_conversation(conv_id);
    check(conv->round == 1, "C: round stays at 1 (not incremented on close)");
}

// ─── Test D: Budget Exceeded -> PAUSED ──────────────────────────────────────

static void test_budget_exceeded() {
    std::cout << "\n=== D. Budget Exceeded -> PAUSED ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test budget", 1.0, 3);

    // Think completes, cost=0.60 — still within budget
    auto think_id = get_pending_task(db, conv_id, "think");
    sui::quorum::ParsedOutput think_out;
    think_out.proposals.push_back({"Budget Proposal", {}, "content"});
    simulate_task_complete(db, engine, think_id, think_out, 0.60);
    check(get_state(db, conv_id) == "reviewing", "D: state = reviewing (within budget)");

    // Review completes, cost=0.50 — total=1.10 exceeds budget=1.0
    auto review_id = get_pending_task(db, conv_id, "review");
    sui::quorum::ParsedOutput review_out;
    review_out.reviews.push_back({"", "approve", "LGTM"});
    simulate_task_complete(db, engine, review_id, review_out, 0.50);

    check(get_state(db, conv_id) == "paused", "D: state = paused (budget exceeded)");
    check(get_paused_reason(db, conv_id).find("budget") != std::string::npos,
          "D: paused_reason contains 'budget'");
}

// ─── Test E: Consecutive Failures -> PAUSED ─────────────────────────────────

static void test_consecutive_failures() {
    std::cout << "\n=== E. Consecutive Failures -> PAUSED ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test failures", 10.0, 3);

    // Mark the think task as failed
    auto think_id = get_pending_task(db, conv_id, "think");
    db.execute(
        "UPDATE tasks SET status = 'failed' WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, think_id);
        }
    );

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

    // Call on_task_complete for the second failed task
    sui::quorum::ParsedOutput empty_parsed;
    auto active = engine.on_task_complete(failed_id2, empty_parsed, 0.05);

    check(!active, "E: on_task_complete returns false (paused)");
    check(get_state(db, conv_id) == "paused", "E: state = paused (consecutive failures)");
    check(get_paused_reason(db, conv_id).find("consecutive failures") != std::string::npos,
          "E: paused_reason contains 'consecutive failures'");
}

// ─── Test F: Agent Escalation -> PAUSED ─────────────────────────────────────

static void test_agent_escalation() {
    std::cout << "\n=== F. Agent Escalation -> PAUSED ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test escalation", 5.0, 3);

    // Think -> review
    auto think_id = get_pending_task(db, conv_id, "think");
    sui::quorum::ParsedOutput think_out;
    think_out.proposals.push_back({"Escalation Proposal", {}, "content"});
    simulate_task_complete(db, engine, think_id, think_out, 0.20);

    // Review with escalate verdict
    auto review_id = get_pending_task(db, conv_id, "review");
    check(review_id > 0, "F: review task created");

    sui::quorum::ParsedOutput review_out;
    review_out.reviews.push_back({"", "escalate", "Conflicting data"});
    simulate_task_complete(db, engine, review_id, review_out, 0.20);

    check(get_state(db, conv_id) == "paused", "F: state = paused (escalation)");
    check(get_paused_reason(db, conv_id).find("escalation") != std::string::npos,
          "F: paused_reason contains 'escalation'");
}

// ─── Test G: Resume from PAUSED ─────────────────────────────────────────────

static void test_resume_from_paused() {
    std::cout << "\n=== G. Resume from PAUSED ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test resume", 1.0, 3);

    // Think completes with high cost -> PAUSED (budget exceeded)
    auto think_id = get_pending_task(db, conv_id, "think");
    sui::quorum::ParsedOutput think_out;
    think_out.proposals.push_back({"Resume Proposal", {}, "content"});
    simulate_task_complete(db, engine, think_id, think_out, 1.10);

    check(get_state(db, conv_id) == "paused", "G: state = paused after budget exceeded");

    // Resume the conversation
    auto ok = engine.resume(conv_id);
    check(ok, "G: resume() returns true");
    check(get_state(db, conv_id) == "thinking", "G: state = thinking after resume");
    check(get_paused_reason(db, conv_id).empty(), "G: paused_reason cleared");

    auto task_count = get_task_count(db, conv_id);
    check(task_count == 2, "G: 2 tasks (original + resumed)");
}

// ─── Test H: Close by Operator ──────────────────────────────────────────────

static void test_close_by_operator() {
    std::cout << "\n=== H. Close by Operator ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test close", 5.0, 3);
    engine.close(conv_id);

    check(get_state(db, conv_id) == "closed", "H: state = closed by operator");
}

// ─── Test I: Reject -> CLOSED ───────────────────────────────────────────────

static void test_reject_closes() {
    std::cout << "\n=== I. Reject -> CLOSED ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test reject", 5.0, 3);

    // Think -> review
    auto think_id = get_pending_task(db, conv_id, "think");
    sui::quorum::ParsedOutput think_out;
    think_out.proposals.push_back({"Reject Proposal", {}, "content"});
    simulate_task_complete(db, engine, think_id, think_out, 0.20);

    // Review with reject verdict
    auto review_id = get_pending_task(db, conv_id, "review");
    check(review_id > 0, "I: review task created");

    sui::quorum::ParsedOutput review_out;
    review_out.reviews.push_back({"", "reject", "Fundamentally flawed"});
    simulate_task_complete(db, engine, review_id, review_out, 0.15);

    check(get_state(db, conv_id) == "closed", "I: state = closed after reject");
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Conversation Pipeline Integration Tests ===\n";

    test_happy_path();
    test_revise_with_session_reuse();
    test_max_rounds_exhausted();
    test_budget_exceeded();
    test_consecutive_failures();
    test_agent_escalation();
    test_resume_from_paused();
    test_close_by_operator();
    test_reject_closes();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
