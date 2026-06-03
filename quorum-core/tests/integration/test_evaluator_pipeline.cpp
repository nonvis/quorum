// tests/integration/test_evaluator_pipeline.cpp
// Phase 8 Track 6 #27 — end-to-end evaluator pipeline.
//
// Wires real OutputParser + ConversationEngine + Database to verify that an
// EVALUATION block emitted by the evaluator agent persists into the
// `evaluations` table with the correct scored/evaluator agent IDs, score
// range, and role_specialty.
//
// Run:  cd build && cmake .. && make test_evaluator_pipeline && ./test_evaluator_pipeline

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <sqlite3.h>
#include <unistd.h>

#include "agent/output_parser.h"
#include "daemon/conversation.h"
#include "storage/database.h"
#include "utils/config.h"

// ---- helpers ----------------------------------------------------------------

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

// Initialize the schema. Mirrors what storage/schema.h::create_schema()
// produces; duplicated here so the integration test doesn't depend on the
// daemon's full init pipeline.
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
        "  system_prompt TEXT"
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
    // Phase 8 Track 3 — evaluations table
    db.execute(
        "CREATE TABLE IF NOT EXISTS evaluations ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  conversation_id INTEGER NOT NULL REFERENCES conversations(id),"
        "  scored_agent_id TEXT NOT NULL,"
        "  evaluator_agent_id TEXT NOT NULL,"
        "  role_specialty TEXT NOT NULL,"
        "  rubric_version TEXT NOT NULL,"
        "  score_total REAL NOT NULL,"
        "  score_json TEXT NOT NULL,"
        "  notes TEXT,"
        "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
        ")"
    );
}

// Build a 3-agent roster: leader, doer, evaluator.
struct Harness {
    sui::quorum::Database db;
    sui::quorum::ConversationConfig cfg;
    std::vector<sui::quorum::AgentMetadata> agents;
    sui::quorum::OutputParser parser;

    Harness() : db(":memory:") {
        init_schema(db);
        cfg.leader = "leader";
        cfg.default_max_rounds = 20;

        agents.push_back(sui::quorum::AgentMetadata{
            .id = "leader", .name = "Leader",
            .description = "Coordinates the team", .role = "leader"
        });
        agents.push_back(sui::quorum::AgentMetadata{
            .id = "doer", .name = "Doer",
            .description = "Implements solutions", .role = "doer"
        });
        agents.push_back(sui::quorum::AgentMetadata{
            .id = "evaluator", .name = "Evaluator",
            .description = "Scores work against rubrics", .role = "evaluator"
        });
    }

    sui::quorum::ConversationEngine make_engine() {
        return sui::quorum::ConversationEngine(db, cfg, agents);
    }

    int64_t pending_task(int64_t conv_id) {
        int64_t id = 0;
        db.query(
            "SELECT id FROM tasks WHERE conversation_id = ? "
            "AND status = 'pending' ORDER BY id DESC LIMIT 1",
            [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, conv_id); },
            [&](sqlite3_stmt* stmt) { id = sqlite3_column_int64(stmt, 0); }
        );
        return id;
    }

    std::string pending_task_agent(int64_t conv_id) {
        std::string agent;
        db.query(
            "SELECT agent FROM tasks WHERE conversation_id = ? "
            "AND status = 'pending' ORDER BY id DESC LIMIT 1",
            [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, conv_id); },
            [&](sqlite3_stmt* stmt) {
                auto a = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (a) agent = a;
            }
        );
        return agent;
    }

    void mark_done(int64_t task_id) {
        db.execute(
            "UPDATE tasks SET status = 'done', completed_at = datetime('now') WHERE id = ?",
            [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, task_id); }
        );
    }
};

// Mirror the wiring in src/main.cpp dispatch: parse output, mark task done,
// run on_task_complete on the engine, and persist any EVALUATION block via
// db.append_evaluation(). The scored_agent_id resolution uses the same
// previous-task-agent fallback the daemon applies.
static void simulate_turn(Harness& h,
                          sui::quorum::ConversationEngine& engine,
                          int64_t task_id,
                          const std::string& raw_output,
                          const std::string& agent_id) {
    auto parsed = h.parser.parse(raw_output);
    h.mark_done(task_id);

    if (parsed.evaluation.has_value()) {
        int64_t conv_id = 0;
        h.db.query(
            "SELECT conversation_id FROM tasks WHERE id = ?",
            [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, task_id); },
            [&](sqlite3_stmt* stmt) {
                conv_id = sqlite3_column_int64(stmt, 0);
            }
        );
        const auto& e = *parsed.evaluation;
        std::string scored = e.scored;
        if (scored.empty()) {
            scored = h.db.previous_task_agent(conv_id, agent_id);
        }
        h.db.append_evaluation(
            conv_id, scored, agent_id, e.role_specialty,
            e.rubric_version, e.total_score, e.items_json, e.notes);
    }

    engine.on_task_complete(task_id, parsed, /*cost=*/0.05);
}

// ---- Test: end-to-end evaluator pipeline ------------------------------------

static void test_evaluator_pipeline_end_to_end() {
    std::cout << "\n=== #27. Evaluator Pipeline E2E ===\n\n";

    namespace fs = std::filesystem;

    Harness h;
    h.cfg.default_path = {"leader", "doer", "evaluator"};

    // Synthesize a tmp project + a stub rubric file so the test mirrors the
    // production setup. The rubric content isn't read by the parser path —
    // its presence is the contract; the evaluator chooses items_json freely.
    auto tdir = fs::temp_directory_path() /
        ("quorum_test_evaluator_" + std::to_string(::getpid()));
    fs::remove_all(tdir);
    fs::create_directories(tdir / "templates" / "rubrics" / "test-specialty");
    {
        std::ofstream f(tdir / "templates" / "rubrics" / "test-specialty" / "rubric.md",
                        std::ios::trunc);
        f << "---\nrole: test-specialty\nversion: v1\n---\n\n"
          << "# Test Specialty Rubric v1\n\n"
          << "- compile-clean (weight 5)\n"
          << "- has-tests (weight 4)\n"
          << "- documented (weight 3)\n";
    }
    h.cfg.target_dir = tdir.string();

    auto engine = h.make_engine();
    auto conv_id = engine.start("Build the thing", 20);

    // Turn 1: leader -> doer
    auto t1 = h.pending_task(conv_id);
    check(h.pending_task_agent(conv_id) == "leader", "#27: turn 1 == leader");
    simulate_turn(h, engine, t1,
        "Plan accepted.\n"
        "\n"
        "```HANDOFF\n"
        "to: doer\n"
        "prompt: implement the thing\n"
        "```\n",
        "leader");

    // Turn 2: doer -> evaluator
    auto t2 = h.pending_task(conv_id);
    check(h.pending_task_agent(conv_id) == "doer", "#27: turn 2 == doer");
    simulate_turn(h, engine, t2,
        "Implementation complete.\n"
        "\n"
        "```HANDOFF\n"
        "to: evaluator\n"
        "prompt: score this work\n"
        "```\n",
        "doer");

    // Turn 3: evaluator emits EVALUATION + HANDOFF to done.
    // Note: `scored:` field intentionally omitted — the daemon should fall
    // back to the previous task agent (doer).
    auto t3 = h.pending_task(conv_id);
    check(h.pending_task_agent(conv_id) == "evaluator", "#27: turn 3 == evaluator");
    std::string evaluator_out =
        "Scored.\n"
        "\n"
        "```EVALUATION\n"
        "role: test-specialty\n"
        "rubric_version: v1\n"
        "total: 67\n"
        "items_json: [{\"id\":\"compile-clean\",\"weight\":5,\"passed\":true},"
            "{\"id\":\"has-tests\",\"weight\":4,\"passed\":false},"
            "{\"id\":\"documented\",\"weight\":3,\"passed\":true}]\n"
        "notes: Compiles cleanly and is documented; missing tests.\n"
        "```\n"
        "\n"
        "```HANDOFF\n"
        "to: done\n"
        "```\n";
    simulate_turn(h, engine, t3, evaluator_out, "evaluator");

    // ── Assertions ─────────────────────────────────────────────────────────

    // A: conversation completed
    auto conv = h.db.get_conversation(conv_id);
    check(conv->state == "done", "#27 A: conversation state == done");

    // B: exactly one evaluation row exists for this conversation
    auto eval_count = h.db.query_int(
        "SELECT COUNT(*) FROM evaluations WHERE conversation_id = " +
        std::to_string(conv_id));
    check(eval_count == 1, "#27 B: 1 evaluation row for this conversation");

    // C, D, E, F: read back the row and verify field values
    struct Row {
        std::string scored_agent_id;
        std::string evaluator_agent_id;
        std::string role_specialty;
        std::string rubric_version;
        double      score_total{0.0};
        std::string score_json;
        std::string notes;
    } row;

    h.db.query(
        "SELECT scored_agent_id, evaluator_agent_id, role_specialty, "
        "rubric_version, score_total, score_json, notes "
        "FROM evaluations WHERE conversation_id = ? LIMIT 1",
        [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, conv_id); },
        [&](sqlite3_stmt* stmt) {
            auto s1 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            row.scored_agent_id = s1 ? s1 : "";
            auto s2 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            row.evaluator_agent_id = s2 ? s2 : "";
            auto s3 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            row.role_specialty = s3 ? s3 : "";
            auto s4 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            row.rubric_version = s4 ? s4 : "";
            row.score_total = sqlite3_column_double(stmt, 4);
            auto s5 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            row.score_json = s5 ? s5 : "";
            auto s6 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            row.notes = s6 ? s6 : "";
        }
    );

    check(row.score_total >= 0.0 && row.score_total <= 100.0,
          "#27 C: score_total in [0, 100]");
    check(row.scored_agent_id == "doer",
          "#27 D: scored_agent_id == 'doer' (fallback to previous task agent)");
    check(row.evaluator_agent_id == "evaluator",
          "#27 E: evaluator_agent_id == 'evaluator'");
    check(row.role_specialty == "test-specialty",
          "#27 F: role_specialty matches EVALUATION block 'role'");

    check(row.rubric_version == "v1", "#27: rubric_version == v1");
    check(row.score_json.find("compile-clean") != std::string::npos,
          "#27: score_json preserves item ids");
    check(row.notes.find("Compiles cleanly") != std::string::npos,
          "#27: notes preserved");

    fs::remove_all(tdir);
}

// ---- main -------------------------------------------------------------------

int main() {
    std::cout << "=== Evaluator Pipeline Integration Tests ===\n";

    test_evaluator_pipeline_end_to_end();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";
    return g_failed > 0 ? 1 : 0;
}
