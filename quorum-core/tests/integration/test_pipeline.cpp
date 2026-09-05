// tests/integration/test_pipeline.cpp
// Integration test for the full Quorum pipeline — exercises criteria 1-7
// without spawning any claude -p processes.
//
// Run:  cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "storage/database.h"
#include "daemon/consensus.h"
#include "agent/invoker.h"
#include "agent/output_parser.h"
#include "agent/context_assembler.h"
#include "vault/vault_manager.h"
#include "utils/config.h"

namespace fs = std::filesystem;

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

static const std::string TEMP_DIR = "/tmp/quorum_pipeline_test";

static void cleanup() {
    std::error_code ec;
    fs::remove_all(TEMP_DIR, ec);
}

// Create tasks table (duplicated from main.cpp since init_schema is static there)
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
        // Columns Invoker::mark_done writes (Phase 7 Track 5 + A4)
        "  cache_creation_input_tokens INTEGER,"
        "  cache_read_input_tokens INTEGER,"
        "  summary TEXT"
        ")"
    );
}

// Duplicate of daily_cost SQL from main.cpp
static double daily_cost(sui::quorum::Database& db) {
    return db.query_double(
        "SELECT COALESCE(SUM(cost), 0.0) FROM tasks "
        "WHERE created_at > datetime('now', '-1 day')"
    );
}

// Duplicate of hourly_cost SQL from main.cpp
static double hourly_cost(sui::quorum::Database& db) {
    return db.query_double(
        "SELECT COALESCE(SUM(cost), 0.0) FROM tasks "
        "WHERE created_at > datetime('now', '-1 hour')"
    );
}

// ─── Test A: Budget enforcement (criterion 7) ───────────────────────────────

static void test_budget_enforcement() {
    std::cout << "\n=== A. Budget Enforcement ===\n\n";

    sui::quorum::Database db(":memory:");
    init_tasks_table(db);

    // Insert 3 done tasks with cost=1.50 each (created_at = now)
    for (int i = 0; i < 3; ++i) {
        db.execute(
            "INSERT INTO tasks (agent, task_type, status, prompt, cost, created_at) "
            "VALUES ('test', 'scan', 'done', 'test', 1.50, datetime('now'))"
        );
    }

    auto d_cost = daily_cost(db);
    auto h_cost = hourly_cost(db);

    check(std::abs(d_cost - 4.50) < 0.01, "daily_cost ~4.50 after 3 tasks");
    check(std::abs(h_cost - 4.50) < 0.01, "hourly_cost ~4.50 after 3 tasks");

    // Insert a task with created_at 2 hours ago
    db.execute(
        "INSERT INTO tasks (agent, task_type, status, prompt, cost, created_at) "
        "VALUES ('test', 'scan', 'done', 'test', 2.00, datetime('now', '-2 hours'))"
    );

    auto d_cost2 = daily_cost(db);
    auto h_cost2 = hourly_cost(db);

    check(std::abs(d_cost2 - 6.50) < 0.01, "daily_cost ~6.50 with old task included");
    check(std::abs(h_cost2 - 4.50) < 0.01, "hourly_cost still ~4.50 (old task excluded)");
}

// ─── Test B: Consensus — create + unanimous approve (criterion 5) ───────────

static void test_consensus_unanimous_approve() {
    std::cout << "\n=== B. Consensus: Unanimous Approve ===\n\n";

    sui::quorum::Database db(":memory:");
    sui::quorum::ConsensusEngine::init_schema(db);
    sui::quorum::ConsensusConfig cfg{.max_rounds = 3};
    sui::quorum::ConsensusEngine consensus(db, cfg);

    auto prop_id = consensus.create_proposal(
        "market_analyst", "Test Proposal", "content",
        {"bot_analyst", "engineer"}, 1);

    auto prop = consensus.get_proposal(prop_id);
    check(prop.has_value(), "B: proposal exists");
    check(prop->state == sui::quorum::ProposalState::Reviewing, "B: state = REVIEWING");
    check(prop->current_round == 1, "B: current_round = 1");

    auto pending = consensus.get_pending_reviewers(prop_id);
    check(pending.size() == 2, "B: 2 pending reviewers");

    bool ok1 = consensus.submit_review(prop_id, "bot_analyst", "approve", "LGTM", 2);
    check(ok1, "B: bot_analyst review accepted");

    pending = consensus.get_pending_reviewers(prop_id);
    check(pending.size() == 1, "B: 1 pending reviewer after bot_analyst");
    check(pending[0] == "engineer", "B: remaining reviewer = engineer");

    bool ok2 = consensus.submit_review(prop_id, "engineer", "approve", "Agreed", 3);
    check(ok2, "B: engineer review accepted");

    prop = consensus.get_proposal(prop_id);
    check(prop->state == sui::quorum::ProposalState::Approved, "B: state = APPROVED");
}

// ─── Test C: Multi-round rejection → escalation ────────────────────────────

static void test_consensus_multiround_escalation() {
    std::cout << "\n=== C. Consensus: Multi-round Rejection -> Escalation ===\n\n";

    sui::quorum::Database db(":memory:");
    sui::quorum::ConsensusEngine::init_schema(db);
    sui::quorum::ConsensusConfig cfg{.max_rounds = 2};
    sui::quorum::ConsensusEngine consensus(db, cfg);

    auto prop_id = consensus.create_proposal(
        "market_analyst", "Risky Trade", "content",
        {"bot_analyst", "engineer"}, 1);

    // Round 1: both reject
    consensus.submit_review(prop_id, "bot_analyst", "reject", "Too risky", 2);
    consensus.submit_review(prop_id, "engineer", "reject", "Disagree", 3);

    auto prop = consensus.get_proposal(prop_id);
    check(prop->state == sui::quorum::ProposalState::Reviewing,
          "C: still REVIEWING after round 1 rejections");
    check(prop->current_round == 2, "C: current_round = 2 after rejection");

    // Round 2: both reject again
    consensus.submit_review(prop_id, "bot_analyst", "reject", "Still too risky", 4);
    consensus.submit_review(prop_id, "engineer", "reject", "Still disagree", 5);

    prop = consensus.get_proposal(prop_id);
    check(prop->state == sui::quorum::ProposalState::Escalated,
          "C: state = ESCALATED after max rounds exhausted");
}

// ─── Test D: Explicit escalate ──────────────────────────────────────────────

static void test_consensus_explicit_escalate() {
    std::cout << "\n=== D. Consensus: Explicit Escalate ===\n\n";

    sui::quorum::Database db(":memory:");
    sui::quorum::ConsensusEngine::init_schema(db);
    sui::quorum::ConsensusConfig cfg{.max_rounds = 3};
    sui::quorum::ConsensusEngine consensus(db, cfg);

    auto prop_id = consensus.create_proposal(
        "market_analyst", "Edge Case", "content",
        {"bot_analyst"}, 1);

    bool ok = consensus.submit_review(prop_id, "bot_analyst", "escalate", "Need human input", 2);
    check(ok, "D: escalate review accepted");

    auto prop = consensus.get_proposal(prop_id);
    check(prop->state == sui::quorum::ProposalState::Escalated,
          "D: state = ESCALATED immediately");
}

// ─── Test E: Duplicate review rejected ──────────────────────────────────────

static void test_consensus_duplicate_review() {
    std::cout << "\n=== E. Consensus: Duplicate Review Rejected ===\n\n";

    sui::quorum::Database db(":memory:");
    sui::quorum::ConsensusEngine::init_schema(db);
    sui::quorum::ConsensusConfig cfg{.max_rounds = 3};
    sui::quorum::ConsensusEngine consensus(db, cfg);

    auto prop_id = consensus.create_proposal(
        "market_analyst", "Test", "content",
        {"bot_analyst", "engineer"}, 1);

    bool ok1 = consensus.submit_review(prop_id, "bot_analyst", "approve", "LGTM", 2);
    check(ok1, "E: first review from bot_analyst accepted");

    bool ok2 = consensus.submit_review(prop_id, "bot_analyst", "approve", "LGTM again", 3);
    check(!ok2, "E: duplicate review from bot_analyst rejected");
}

// ─── Test F: Full pipeline (criteria 3, 4, 5 combined) ─────────────────────

static void test_full_pipeline() {
    std::cout << "\n=== F. Full Pipeline ===\n\n";

    // Setup: temp dir, vault manager, DB, consensus, parser, assembler
    cleanup();
    fs::create_directories(TEMP_DIR);

    sui::quorum::Database db(":memory:");
    init_tasks_table(db);
    sui::quorum::ConsensusEngine::init_schema(db);
    sui::quorum::ConsensusConfig cfg{.max_rounds = 3};
    sui::quorum::ConsensusEngine consensus(db, cfg);
    sui::quorum::OutputParser parser;
    sui::quorum::ContextAssembler assembler;
    sui::quorum::VaultManager vault_manager(TEMP_DIR);

    // Step 1: Init vaults and seed CONTEXT.md for both agents
    (void)vault_manager.init_vault("market_analyst");
    (void)vault_manager.init_vault("bot_analyst");

    {
        auto ctx_path = fs::path(vault_manager.vault_path("market_analyst")) / "CONTEXT.md";
        std::ofstream ctx(ctx_path);
        ctx << "You are the Market Analyst agent.\n";
    }
    {
        auto ctx_path = fs::path(vault_manager.vault_path("bot_analyst")) / "CONTEXT.md";
        std::ofstream ctx(ctx_path);
        ctx << "You are the Bot Analyst agent.\n";
    }

    // Step 2: Parse canned agent output containing VAULT_UPDATE + PROPOSAL
    std::string canned_output =
        "Here is my analysis.\n"
        "\n"
        "```VAULT_UPDATE\n"
        "path: knowledge/spread-analysis.md\n"
        "content: |\n"
        "  SUI/USDC spread averaging 5bps. Opportunity detected.\n"
        "```\n"
        "\n"
        "```PROPOSAL\n"
        "title: Tighten SUI/USDC Spread\n"
        "requires_consensus_from: [bot_analyst]\n"
        "content: |\n"
        "  Reduce spread from 8bps to 5bps based on market analysis.\n"
        "```\n"
        "\n"
        "```SUMMARY\n"
        "Analyzed SUI/USDC pool. Proposing spread tightening.\n"
        "```";

    auto parsed = parser.parse(canned_output);
    check(parsed.vault_updates.size() == 1, "F2: 1 vault_update parsed");
    check(parsed.proposals.size() == 1, "F2: 1 proposal parsed");
    check(!parsed.summary.empty(), "F2: summary present");

    // Step 3: Apply vault update (criterion 4 — writes persist)
    auto& vu = parsed.vault_updates[0];
    bool vu_ok = vault_manager.apply_vault_update("market_analyst", vu);
    check(vu_ok, "F3: vault update applied");

    auto file_content = vault_manager.read_file("market_analyst", "knowledge/spread-analysis.md");
    check(file_content.has_value(), "F3: spread-analysis.md exists");
    check(file_content->find("5bps") != std::string::npos, "F3: file content contains '5bps'");

    // Step 4: Create proposal from parsed fields (criterion 5)
    auto& p = parsed.proposals[0];
    auto prop_id = consensus.create_proposal(
        "market_analyst", p.title, p.content,
        p.requires_consensus_from, 1);

    auto prop = consensus.get_proposal(prop_id);
    check(prop.has_value(), "F4: proposal created");
    check(prop->state == sui::quorum::ProposalState::Reviewing, "F4: proposal state = REVIEWING");

    // Step 5: Re-assemble context — verify vault content persists (criterion 4)
    auto vault_dir = vault_manager.vault_path("market_analyst");
    auto prompt = assembler.assemble("market_analyst", vault_dir, "scan", "Check markets");
    check(prompt.find("5bps") != std::string::npos,
          "F5: re-assembled context contains persisted vault content");

    // Step 6: Parse canned reviewer output containing REVIEW
    std::string review_output =
        "I've reviewed the proposal.\n"
        "\n"
        "```REVIEW\n"
        "proposal_id: " + prop_id + "\n"
        "verdict: approve\n"
        "reasoning: |\n"
        "  Spread analysis looks sound. 5bps is competitive.\n"
        "```\n"
        "\n"
        "Approved spread tightening proposal.";

    auto review_parsed = parser.parse(review_output);
    check(review_parsed.reviews.size() == 1, "F6: 1 review parsed");
    check(review_parsed.reviews[0].proposal_id == prop_id, "F6: review proposal_id matches");
    check(review_parsed.reviews[0].verdict == "approve", "F6: review verdict = approve");

    // Step 7: Submit review — proposal should transition to APPROVED (criterion 5)
    auto& r = review_parsed.reviews[0];
    bool rev_ok = consensus.submit_review(r.proposal_id, "bot_analyst", r.verdict, r.reasoning, 2);
    check(rev_ok, "F7: review submitted successfully");

    prop = consensus.get_proposal(prop_id);
    check(prop->state == sui::quorum::ProposalState::Approved,
          "F7: proposal state = APPROVED (full cycle complete)");

    cleanup();
}

// ─── Test G: Sequential dispatch (criterion 6) ──────────────────────────────

static void test_sequential_dispatch() {
    std::cout << "\n=== G. Sequential Dispatch ===\n\n";

    sui::quorum::Database db(":memory:");
    init_tasks_table(db);

    // No active tasks — dispatch should proceed
    auto active0 = db.query_int("SELECT COUNT(*) FROM tasks WHERE status = 'active'");
    check(active0 == 0, "G: 0 active tasks — dispatch allowed");

    // Insert 1 active task — dispatch should block
    db.execute(
        "INSERT INTO tasks (agent, task_type, status, prompt) "
        "VALUES ('test', 'scan', 'active', 'running task')"
    );
    auto active1 = db.query_int("SELECT COUNT(*) FROM tasks WHERE status = 'active'");
    check(active1 == 1, "G: 1 active task counted");
    check(active1 > 0, "G: sequential gate blocks (active > 0)");

    // Insert more active tasks — still blocks (same condition)
    db.execute(
        "INSERT INTO tasks (agent, task_type, status, prompt) "
        "VALUES ('test', 'scan', 'active', 'running task 2')"
    );
    auto active2 = db.query_int("SELECT COUNT(*) FROM tasks WHERE status = 'active'");
    check(active2 > 0, "G: sequential gate still blocks with 2 active");
}

// ─── Test H: tasks.summary completion round trip (A4) ───────────────────────
//
// Drives the REAL completion write — Invoker::mark_done, the only
// `UPDATE tasks SET status = 'done'` site in src/ and the one call `invoke()`
// makes after a healthy envelope — with a fake agent output, then reads the
// row back. No claude -p is spawned: mark_done is pure DB.

// Read tasks.summary for a row. Returns {found_non_null, value}.
static std::pair<bool, std::string> read_summary(sui::quorum::Database& db,
                                                 int64_t task_id) {
    std::pair<bool, std::string> out{false, {}};
    db.query(
        "SELECT summary FROM tasks WHERE id = ?",
        [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, task_id); },
        [&](sqlite3_stmt* stmt) {
            if (sqlite3_column_type(stmt, 0) == SQLITE_NULL) return;
            out.first = true;
            auto t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (t) out.second = t;
        }
    );
    return out;
}

static int64_t insert_active_task(sui::quorum::Database& db) {
    db.execute(
        "INSERT INTO tasks (agent, task_type, status, prompt) "
        "VALUES ('doer', 'work', 'active', 'do the thing')"
    );
    return db.last_insert_id();
}

static void test_summary_completion_round_trip() {
    std::cout << "\n=== H. tasks.summary Completion Round Trip ===\n\n";

    sui::quorum::Database db(":memory:");
    init_tasks_table(db);
    sui::quorum::Invoker invoker(db);

    // ── 1. Output carrying an explicit verdict ──────────────────────────────
    auto with_verdict = insert_active_task(db);
    const std::string verdict_output =
        "Ran the migration and the suite.\n\nVERDICT: shipped X\n";
    invoker.mark_done(with_verdict, verdict_output, 10, 20, 0.5, 0, 0);

    auto done = db.query_int(
        "SELECT COUNT(*) FROM tasks WHERE id = " + std::to_string(with_verdict) +
        " AND status = 'done'");
    check(done == 1, "H: mark_done marked the task done");

    auto got = read_summary(db, with_verdict);
    check(got.first, "H: summary column is non-NULL for a VERDICT output");
    check(got.second == "shipped X",
          "H: SELECT summary returns the daemon-extracted verdict");

    // ── 2. Output with no verdict anywhere ──────────────────────────────────
    auto no_verdict = insert_active_task(db);
    invoker.mark_done(no_verdict,
                      "I looked at the code and everything seems fine\n",
                      10, 20, 0.5, 0, 0);

    auto is_null = db.query_int(
        "SELECT COUNT(*) FROM tasks WHERE id = " + std::to_string(no_verdict) +
        " AND summary IS NULL");
    check(is_null == 1, "H: no verdict → summary IS NULL (absent, not empty)");

    auto is_empty_string = db.query_int(
        "SELECT COUNT(*) FROM tasks WHERE id = " + std::to_string(no_verdict) +
        " AND summary = ''");
    check(is_empty_string == 0, "H: no verdict → summary is never \"\"");

    // The result itself still lands — the summary is additive, not a swap.
    auto result_kept = db.query_int(
        "SELECT COUNT(*) FROM tasks WHERE id = " + std::to_string(no_verdict) +
        " AND result LIKE 'I looked at the code%'");
    check(result_kept == 1, "H: the raw result is still persisted alongside");
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Integration Pipeline Tests ===\n";

    test_budget_enforcement();
    test_consensus_unanimous_approve();
    test_consensus_multiround_escalation();
    test_consensus_explicit_escalate();
    test_consensus_duplicate_review();
    test_full_pipeline();
    test_sequential_dispatch();
    test_summary_completion_round_trip();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
