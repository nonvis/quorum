// tests/integration/test_benchmark_runner.cpp
// Phase 8 Track 6 #28 — rubric override integration test.
//
// Verifies that when both a project-level rubric (.quorum/rubrics/<rs>/
// rubric.md) and a template rubric (templates/rubrics/<rs>/rubric.md)
// exist, the project override wins. The evaluator persists items_json
// referencing the OVERRIDE rubric's item IDs, not the template's.
//
// We use a synthetic role-specialty `test-specialty` whose template lives
// only inside the temp staging tree we set up in the test (so the
// resolver's CWD-ladder fallback hits it). The override goes under
// .quorum/rubrics/. Expected behavior: the score_json column on the
// evaluations row references the override's item IDs.
//
// Mirrors the harness pattern from test_evaluator_pipeline.cpp: real
// OutputParser + ConversationEngine + Database, simulated agent outputs
// (no claude -p invocation).

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <sqlite3.h>
#include <unistd.h>

#include "agent/output_parser.h"
#include "agent/rubric.h"
#include "daemon/conversation.h"
#include "storage/database.h"

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

// Mirror the schema portions touched by this test.
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

struct Harness {
    sui::quorum::Database db;
    sui::quorum::ConversationConfig cfg;
    std::vector<sui::quorum::AgentMetadata> agents;
    sui::quorum::OutputParser parser;

    Harness() : db(":memory:") {
        init_schema(db);
        cfg.leader = "leader";
        cfg.default_max_rounds = 20;
        cfg.default_budget_usd = 5.0;

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

    void mark_done(int64_t task_id) {
        db.execute(
            "UPDATE tasks SET status = 'done', "
            "completed_at = datetime('now') WHERE id = ?",
            [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, task_id); }
        );
    }
};

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

// ---- Test: rubric override wins over template default ----------------------

// Strategy:
//   1. Stage a temp dir with BOTH a template rubric AND a project override.
//      Template:  <tdir>/templates/rubrics/test-specialty/rubric.md  (v1)
//      Override:  <tdir>/.quorum/rubrics/test-specialty/rubric.md     (v2)
//   2. The two rubrics have DIFFERENT item IDs (and a different version
//      string in frontmatter), so we can tell them apart from the
//      score_json that lands.
//   3. Verify resolve_rubric() picks the override (project_root != "").
//   4. Drive a leader → doer → evaluator conversation. The evaluator's
//      EVALUATION block emits items_json keyed to the OVERRIDE rubric's
//      item IDs. After persistence, the score_json column reflects those
//      override IDs.
//   5. Negative control: resolve_rubric() with project_root="" must pick
//      the template instead, returning version v1.
static void test_rubric_override_wins() {
    std::cout << "\n=== #28. Rubric Override Integration ===\n\n";

    namespace fs = std::filesystem;

    auto tdir = fs::temp_directory_path() /
        ("quorum_test_benchmark_" + std::to_string(::getpid()));
    fs::remove_all(tdir);

    // Template at <tdir>/templates/rubrics/test-specialty/rubric.md
    auto tmpl_dir = tdir / "templates" / "rubrics" / "test-specialty";
    fs::create_directories(tmpl_dir);
    {
        std::ofstream f(tmpl_dir / "rubric.md", std::ios::trunc);
        f << "---\nname: test-specialty\nversion: v1\n---\n\n"
          << "# Test Specialty Template (v1)\n\n"
          << "## Template Category\n"
          << "- [ ] (5) template-only-item-alpha\n"
          << "- [ ] (5) template-only-item-beta\n";
    }

    // Project override at <tdir>/.quorum/rubrics/test-specialty/rubric.md
    auto proj_root = tdir / "project";
    auto override_dir = proj_root / ".quorum" / "rubrics" / "test-specialty";
    fs::create_directories(override_dir);
    {
        std::ofstream f(override_dir / "rubric.md", std::ios::trunc);
        f << "---\nname: test-specialty\nversion: v2\n---\n\n"
          << "# Test Specialty Override (v2)\n\n"
          << "## Override Category\n"
          << "- [ ] (10) project-override-item-gamma\n"
          << "- [ ] (10) project-override-item-delta\n";
    }

    // Run resolve from inside <tdir>/project so the CWD-relative
    // template ladder finds <tdir>/templates/... via the parent path,
    // and the project_root override lookup hits the project's
    // .quorum/rubrics/.
    auto saved_cwd = fs::current_path();
    fs::current_path(proj_root);

    // Sanity: template-only path resolves to v1.
    auto template_only = sui::quorum::resolve_rubric("", "test-specialty");
    check(template_only.has_value(),
          "#28: template-only resolution returns a rubric");
    check(template_only && template_only->version == "v1",
          "#28: template-only resolution returns v1");

    // Override path resolves to v2.
    auto with_override = sui::quorum::resolve_rubric(
        proj_root.string(), "test-specialty");
    check(with_override.has_value(),
          "#28: override path resolution returns a rubric");
    check(with_override && with_override->version == "v2",
          "#28 A: override resolution returns v2 (NOT v1 from template)");

    // Spot-check the override's items came from the override rubric, not
    // the template. The override rubric uses unique item IDs so we can
    // tell them apart.
    bool found_override_item = false;
    bool found_template_item = false;
    if (with_override) {
        for (const auto& it : with_override->items) {
            if (it.id.find("project-override-item") != std::string::npos)
                found_override_item = true;
            if (it.id.find("template-only-item") != std::string::npos)
                found_template_item = true;
        }
    }
    check(found_override_item,
          "#28 B: override rubric contributes its items to the resolved set");
    check(!found_template_item,
          "#28 C: template items do NOT leak into the override resolution");

    // ── Drive the conversation ───────────────────────────────────────────

    Harness h;
    h.cfg.default_path = {"leader", "doer", "evaluator"};

    auto engine = h.make_engine();
    auto conv_id = engine.start("Build the thing", 5.0, 20);

    // Turn 1: leader -> doer
    auto t1 = h.pending_task(conv_id);
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
    simulate_turn(h, engine, t2,
        "Implementation complete.\n"
        "\n"
        "```HANDOFF\n"
        "to: evaluator\n"
        "prompt: score this work\n"
        "```\n",
        "doer");

    // Turn 3: evaluator emits EVALUATION with the OVERRIDE rubric's IDs.
    // (This is the contract: when scoring, the evaluator references the
    // resolved rubric's item IDs. resolve_rubric picks the override, so
    // the items_json IDs come from the override.)
    auto t3 = h.pending_task(conv_id);
    std::string evaluator_out =
        "Scored against override rubric (v2).\n"
        "\n"
        "```EVALUATION\n"
        "role: test-specialty\n"
        "rubric_version: v2\n"
        "total: 75\n"
        "items_json: ["
            "{\"id\":\"override-category.project-override-item-gamma\","
                "\"weight\":10,\"passed\":true},"
            "{\"id\":\"override-category.project-override-item-delta\","
                "\"weight\":10,\"passed\":false}"
        "]\n"
        "notes: Scored using project rubric override.\n"
        "```\n"
        "\n"
        "```HANDOFF\n"
        "to: done\n"
        "```\n";
    simulate_turn(h, engine, t3, evaluator_out, "evaluator");

    // ── Verify persistence ───────────────────────────────────────────────

    auto eval_count = h.db.query_int(
        "SELECT COUNT(*) FROM evaluations WHERE conversation_id = " +
        std::to_string(conv_id));
    check(eval_count == 1, "#28 D: 1 evaluation row persisted");

    std::string score_json;
    std::string rubric_version_in_row;
    h.db.query(
        "SELECT score_json, rubric_version FROM evaluations "
        "WHERE conversation_id = ? LIMIT 1",
        [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, conv_id); },
        [&](sqlite3_stmt* stmt) {
            auto s1 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            score_json = s1 ? s1 : "";
            auto s2 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            rubric_version_in_row = s2 ? s2 : "";
        }
    );

    check(rubric_version_in_row == "v2",
          "#28 E: persisted rubric_version == v2 (from override)");

    check(score_json.find("project-override-item-gamma") != std::string::npos,
          "#28 F: score_json references override rubric's gamma item");
    check(score_json.find("project-override-item-delta") != std::string::npos,
          "#28 G: score_json references override rubric's delta item");
    check(score_json.find("template-only-item-alpha") == std::string::npos,
          "#28 H: score_json does NOT reference template's alpha item");
    check(score_json.find("template-only-item-beta") == std::string::npos,
          "#28 I: score_json does NOT reference template's beta item");

    fs::current_path(saved_cwd);
    fs::remove_all(tdir);
}

// ---- main -------------------------------------------------------------------

int main() {
    std::cout << "=== Benchmark Runner Integration Tests ===\n";

    test_rubric_override_wins();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";
    return g_failed > 0 ? 1 : 0;
}
