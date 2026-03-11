// tests/integration/test_executor_pipeline.cpp
// Integration test for the Executor Pipeline — exercises the full
// INIT -> THINKING -> APPROVED -> EXECUTING -> REVIEWING -> DONE state machine
// end-to-end without spawning any claude -p processes.
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
#include "utils/config.h"

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

static std::string get_task_prompt(sui::quorum::Database& db, int64_t conv_id,
                                   const std::string& task_type) {
    std::string prompt;
    db.query(
        "SELECT prompt FROM tasks WHERE conversation_id = ? AND task_type = ? "
        "ORDER BY id DESC LIMIT 1",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, conv_id);
            sqlite3_bind_text(stmt, 2, task_type.c_str(), -1, SQLITE_TRANSIENT);
        },
        [&](sqlite3_stmt* stmt) {
            auto s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (s) prompt = s;
        }
    );
    return prompt;
}

// ─── Test A: Executor Happy Path ─────────────────────────────────────────────
// Full path: INIT → THINKING → APPROVED → EXECUTING → REVIEWING → DONE

static void test_executor_happy_path() {
    std::cout << "\n=== A. Executor Happy Path ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Create a hello world in C++", 5.0, 3, "executor");
    check(get_state(db, conv_id) == "thinking", "A: state = thinking after start");

    // Verify pipeline = executor
    auto conv = db.get_conversation(conv_id);
    check(conv->pipeline == "executor", "A: pipeline = executor");

    // Get think task, verify agent
    auto think_id = get_pending_task(db, conv_id, "think");
    check(think_id > 0, "A: think task created");
    check(get_task_agent(db, think_id) == "thinker", "A: think task agent = thinker");

    // Think completes — must set result before on_task_complete (gate() reads it)
    sui::quorum::ParsedOutput think_out;
    think_out.proposals.push_back({"Hello World Plan", {}, "1. Create main.cpp\n2. Compile\n3. Run"});
    db.execute(
        "UPDATE tasks SET status = 'done', cost = 0.30, result = 'Create main.cpp with hello world' WHERE id = ?",
        [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, think_id); }
    );
    engine.on_task_complete(think_id, think_out, 0.30);
    check(get_state(db, conv_id) == "approved", "A: state = approved after think (executor pipeline)");

    // Human gate: approve
    bool gate_ok = engine.gate(conv_id, true);
    check(gate_ok, "A: gate(approve) returns true");
    check(get_state(db, conv_id) == "executing", "A: state = executing after gate approve");

    // Verify execute task
    auto exec_id = get_pending_task(db, conv_id, "execute");
    check(exec_id > 0, "A: execute task created");
    check(get_task_agent(db, exec_id) == "executor", "A: execute task agent = executor");

    // Executor completes — must set result before on_task_complete (handle_executing reads it)
    sui::quorum::ParsedOutput empty_parsed;
    db.execute(
        "UPDATE tasks SET status = 'done', cost = 0.50, result = 'Created main.cpp, compiled, runs OK' WHERE id = ?",
        [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, exec_id); }
    );
    engine.on_task_complete(exec_id, empty_parsed, 0.50);
    check(get_state(db, conv_id) == "reviewing", "A: state = reviewing after execute");

    // Verify review task
    auto review_id = get_pending_task(db, conv_id, "review");
    check(review_id > 0, "A: review task created");
    check(get_task_agent(db, review_id) == "reviewer", "A: review task agent = reviewer");

    // Reviewer approves
    sui::quorum::ParsedOutput review_out;
    review_out.reviews.push_back({"", "approve", "Implementation matches plan"});
    simulate_task_complete(db, engine, review_id, review_out, 0.25);
    check(get_state(db, conv_id) == "done", "A: state = done after approve");

    conv = db.get_conversation(conv_id);
    check(std::abs(conv->spent_usd - 1.05) < 0.01, "A: spent_usd ~= 1.05");
}

// ─── Test B: Gate Reject ─────────────────────────────────────────────────────
// APPROVED → gate(false) → CLOSED

static void test_gate_reject() {
    std::cout << "\n=== B. Gate Reject ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Build a widget", 5.0, 3, "executor");

    // Think completes with proposal
    auto think_id = get_pending_task(db, conv_id, "think");
    sui::quorum::ParsedOutput think_out;
    think_out.proposals.push_back({"Widget Plan", {}, "steps here"});
    db.execute(
        "UPDATE tasks SET status = 'done', cost = 0.20, result = 'Widget proposal' WHERE id = ?",
        [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, think_id); }
    );
    engine.on_task_complete(think_id, think_out, 0.20);
    check(get_state(db, conv_id) == "approved", "B: state = approved");

    // Reject at gate
    bool gate_ok = engine.gate(conv_id, false);
    check(gate_ok, "B: gate(reject) returns true");
    check(get_state(db, conv_id) == "closed", "B: state = closed after gate reject");

    // No execute tasks should exist
    auto exec_id = get_pending_task(db, conv_id, "execute");
    check(exec_id == 0, "B: no execute task created");
}

// ─── Test C: Gate Wrong State ────────────────────────────────────────────────
// gate() on non-APPROVED state returns false

static void test_gate_wrong_state() {
    std::cout << "\n=== C. Gate Wrong State ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Test gate timing", 5.0, 3, "executor");
    check(get_state(db, conv_id) == "thinking", "C: state = thinking");

    // Try gate while still thinking
    bool gate_ok = engine.gate(conv_id, true);
    check(!gate_ok, "C: gate returns false (wrong state)");
    check(get_state(db, conv_id) == "thinking", "C: state still thinking after failed gate");
}

// ─── Test D: Executor Reviewer Reject → CLOSED ──────────────────────────────

static void test_executor_reviewer_reject() {
    std::cout << "\n=== D. Executor Reviewer Reject ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Build something", 5.0, 3, "executor");

    // Think → approved
    auto think_id = get_pending_task(db, conv_id, "think");
    sui::quorum::ParsedOutput think_out;
    think_out.proposals.push_back({"Build Plan", {}, "steps"});
    db.execute(
        "UPDATE tasks SET status = 'done', cost = 0.20, result = 'Build plan details' WHERE id = ?",
        [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, think_id); }
    );
    engine.on_task_complete(think_id, think_out, 0.20);

    // Gate approve → executing
    engine.gate(conv_id, true);

    // Executor completes
    auto exec_id = get_pending_task(db, conv_id, "execute");
    db.execute(
        "UPDATE tasks SET status = 'done', cost = 0.40, result = 'Implemented but with bugs' WHERE id = ?",
        [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, exec_id); }
    );
    sui::quorum::ParsedOutput empty_parsed;
    engine.on_task_complete(exec_id, empty_parsed, 0.40);
    check(get_state(db, conv_id) == "reviewing", "D: state = reviewing");

    // Reviewer rejects
    auto review_id = get_pending_task(db, conv_id, "review");
    sui::quorum::ParsedOutput review_out;
    review_out.reviews.push_back({"", "reject", "Code has compilation errors"});
    simulate_task_complete(db, engine, review_id, review_out, 0.15);
    check(get_state(db, conv_id) == "closed", "D: state = closed after reject");
}

// ─── Test E: Pipeline Selection ──────────────────────────────────────────────
// Same engine, both pipelines side by side

static void test_pipeline_selection() {
    std::cout << "\n=== E. Pipeline Selection ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    // Start both pipelines
    auto analyst_id = engine.start("Analyze performance", 5.0, 3, "analyst");
    auto executor_id = engine.start("Implement feature", 5.0, 3, "executor");

    // Complete thinker for analyst conversation
    auto analyst_think = get_pending_task(db, analyst_id, "think");
    sui::quorum::ParsedOutput think_out;
    think_out.proposals.push_back({"Analysis", {}, "findings"});
    simulate_task_complete(db, engine, analyst_think, think_out, 0.20);

    // Complete thinker for executor conversation
    auto executor_think = get_pending_task(db, executor_id, "think");
    sui::quorum::ParsedOutput think_out2;
    think_out2.proposals.push_back({"Feature Plan", {}, "steps"});
    db.execute(
        "UPDATE tasks SET status = 'done', cost = 0.20, result = 'Feature plan' WHERE id = ?",
        [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, executor_think); }
    );
    engine.on_task_complete(executor_think, think_out2, 0.20);

    // Analyst should be in reviewing, executor should be in approved
    check(get_state(db, analyst_id) == "reviewing",
          "E: analyst conv -> reviewing after think");
    check(get_state(db, executor_id) == "approved",
          "E: executor conv -> approved after think");
}

// ─── Test F: Agent Class Routing ─────────────────────────────────────────────
// AgentMetadata struct defaults

static void test_agent_class_routing() {
    std::cout << "\n=== F. Agent Class Routing ===\n\n";

    // Analyst agent
    sui::quorum::AgentMetadata analyst;
    analyst.id = "test_analyst";
    analyst.agent_class = "analyst";
    check(analyst.agent_class != "executor", "F: analyst agent_class != executor");

    // Executor agent with target_dir
    sui::quorum::AgentMetadata exec;
    exec.id = "test_executor";
    exec.agent_class = "executor";
    exec.target_dir = "~/projects/hello-world";
    check(exec.agent_class == "executor", "F: executor agent_class == executor");
    check(!exec.target_dir.empty(), "F: executor target_dir non-empty");

    // Default agent (only id set)
    sui::quorum::AgentMetadata default_agent;
    default_agent.id = "some_agent";
    check(default_agent.agent_class == "analyst", "F: default agent_class == analyst");
}

// ─── Test G: Reviewer Gets Context ──────────────────────────────────────────
// Reviewer prompt contains both thinker and executor output

static void test_reviewer_gets_context() {
    std::cout << "\n=== G. Reviewer Gets Context ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    auto conv_id = engine.start("Build hello world", 5.0, 3, "executor");

    // Think completes with specific result text
    auto think_id = get_pending_task(db, conv_id, "think");
    sui::quorum::ParsedOutput think_out;
    think_out.proposals.push_back({"HW Plan", {}, "Create main.cpp with printf"});
    std::string thinker_result = "THINKER_UNIQUE_RESULT_12345";
    db.execute(
        "UPDATE tasks SET status = 'done', cost = 0.20, result = ? WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_text(stmt, 1, thinker_result.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 2, think_id);
        }
    );
    engine.on_task_complete(think_id, think_out, 0.20);

    // Gate approve
    engine.gate(conv_id, true);

    // Executor completes with specific result text
    auto exec_id = get_pending_task(db, conv_id, "execute");
    std::string executor_result = "EXECUTOR_UNIQUE_RESULT_67890";
    db.execute(
        "UPDATE tasks SET status = 'done', cost = 0.30, result = ? WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_text(stmt, 1, executor_result.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 2, exec_id);
        }
    );
    sui::quorum::ParsedOutput empty_parsed;
    engine.on_task_complete(exec_id, empty_parsed, 0.30);

    // Check the reviewer task's prompt
    auto prompt = get_task_prompt(db, conv_id, "review");
    check(prompt.find("Original Plan") != std::string::npos,
          "G: reviewer prompt contains 'Original Plan'");
    check(prompt.find("Executor Output") != std::string::npos,
          "G: reviewer prompt contains 'Executor Output'");
    check(prompt.find(thinker_result) != std::string::npos,
          "G: reviewer prompt contains thinker's result text");
    check(prompt.find(executor_result) != std::string::npos,
          "G: reviewer prompt contains executor's result text");
}

// ─── Test H: Resume Executor from Paused ─────────────────────────────────────
// Executor task exceeds budget → paused → resume creates new execute task

static void test_resume_executor() {
    std::cout << "\n=== H. Resume Executor from Paused ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);
    sui::quorum::ConversationEngine engine(db);

    // Use tight budget
    auto conv_id = engine.start("Build something small", 1.0, 3, "executor");

    // Think completes (within budget)
    auto think_id = get_pending_task(db, conv_id, "think");
    sui::quorum::ParsedOutput think_out;
    think_out.proposals.push_back({"Small Plan", {}, "steps"});
    db.execute(
        "UPDATE tasks SET status = 'done', cost = 0.20, result = 'Small plan details' WHERE id = ?",
        [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, think_id); }
    );
    engine.on_task_complete(think_id, think_out, 0.20);

    // Gate approve
    engine.gate(conv_id, true);

    // Executor completes with cost that exceeds budget (0.20 + 0.90 = 1.10 > 1.0)
    auto exec_id = get_pending_task(db, conv_id, "execute");
    auto orig_session = get_session_id(db, exec_id);
    db.execute(
        "UPDATE tasks SET status = 'done', cost = 0.90, result = 'Partial work' WHERE id = ?",
        [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, exec_id); }
    );
    sui::quorum::ParsedOutput empty_parsed;
    engine.on_task_complete(exec_id, empty_parsed, 0.90);
    check(get_state(db, conv_id) == "paused", "H: state = paused (budget exceeded)");

    // Resume
    auto ok = engine.resume(conv_id);
    check(ok, "H: resume() returns true");
    check(get_state(db, conv_id) == "executing", "H: state = executing after resume");
    check(get_paused_reason(db, conv_id).empty(), "H: paused_reason cleared");

    // Verify new execute task with same session_id
    auto new_exec_id = get_pending_task(db, conv_id, "execute");
    check(new_exec_id > 0, "H: new execute task created");
    check(new_exec_id != exec_id, "H: new task is different from original");
    auto new_session = get_session_id(db, new_exec_id);
    check(!orig_session.empty(), "H: original session_id non-empty");
    check(orig_session == new_session, "H: session_id reused on resume");
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Executor Pipeline Integration Tests ===\n";

    test_executor_happy_path();
    test_gate_reject();
    test_gate_wrong_state();
    test_executor_reviewer_reject();
    test_pipeline_selection();
    test_agent_class_routing();
    test_reviewer_gets_context();
    test_resume_executor();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
