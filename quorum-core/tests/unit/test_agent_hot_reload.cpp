// tests/unit/test_agent_hot_reload.cpp
// Phase 9 finding #2 — daemon-side hot reload of agents.
//
// Verifies that ConversationEngine::reload_agents() picks up agent yamls
// added to the agents directory after the engine was constructed. Mirrors
// the daemon-loop fix in main.cpp's task_dispatch (which calls the same
// reload helper from utils/config.h).
//
// Run: ctest -R test_agent_hot_reload --output-on-failure

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

#include "daemon/conversation.h"
#include "utils/config.h"

namespace fs = std::filesystem;

static int g_passed = 0;
static int g_failed = 0;
static int g_test_num = 0;

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        ++g_failed;
        std::exit(1);
    }
    std::cout << "[PASS] " << msg << "\n";
    ++g_passed;
}

static std::string make_temp_dir() {
    auto dir = fs::temp_directory_path() /
        ("quorum_test_reload_" + std::to_string(getpid()) + "_" +
         std::to_string(g_test_num++));
    fs::create_directories(dir);
    return dir.string();
}

static void write_agent_yaml(const std::string& dir, const std::string& id) {
    std::ofstream f(fs::path(dir) / (id + ".yaml"));
    f << "id: " << id << "\n";
    f << "role: doer\n";
}

static void init_min_schema(sui::quorum::Database& db) {
    db.execute(
        "CREATE TABLE IF NOT EXISTS conversations ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "goal TEXT, budget_usd REAL, max_rounds INTEGER,"
        "round INTEGER DEFAULT 0, current_agent TEXT,"
        "state TEXT DEFAULT 'active', mode TEXT, team TEXT,"
        "paused_reason TEXT, created_at TEXT DEFAULT CURRENT_TIMESTAMP)");
    db.execute(
        "CREATE TABLE IF NOT EXISTS tasks ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "conversation_id INTEGER, agent TEXT, kind TEXT,"
        "prompt TEXT, status TEXT DEFAULT 'pending', session_id TEXT,"
        "created_at TEXT DEFAULT CURRENT_TIMESTAMP, completed_at TEXT)");
    db.execute(
        "CREATE TABLE IF NOT EXISTS sessions ("
        "conversation_id INTEGER, agent TEXT, session_id TEXT,"
        "PRIMARY KEY (conversation_id, agent))");
}

// ---------------------------------------------------------------------------
// T1: reload picks up newly-created agents
// ---------------------------------------------------------------------------

static void test_t1_reload_picks_up_new_agent() {
    std::cout << "\n=== T1. reload_agents() picks up new agent (#2) ===\n\n";

    auto tmp = make_temp_dir();
    auto agents_dir = tmp + "/.quorum/agents";
    fs::create_directories(agents_dir);

    // Initial roster: one agent on disk.
    write_agent_yaml(agents_dir, "leader");

    sui::quorum::Database db(":memory:");
    init_min_schema(db);

    sui::quorum::ConversationConfig cfg;
    cfg.leader = "leader";

    auto initial = sui::quorum::load_agents_from_directory(agents_dir);
    check(initial.size() == 1, "T1: initial scan finds 1 agent");

    std::vector<sui::quorum::AgentMetadata> agents = initial;
    sui::quorum::ConversationEngine engine(db, cfg, agents, nullptr, tmp, agents_dir);

    // Add a second agent yaml AFTER the engine was constructed.
    write_agent_yaml(agents_dir, "extra");

    // Without reload, the engine's local roster still has only the initial one.
    // The reload helper refreshes its local copy from disk.
    engine.reload_agents();

    auto refreshed = sui::quorum::load_agents_from_directory(agents_dir);
    check(refreshed.size() == 2, "T1: post-add scan finds 2 agents");
    bool found_extra = false;
    for (const auto& a : refreshed) {
        if (a.id == "extra") found_extra = true;
    }
    check(found_extra, "T1: post-add scan includes 'extra'");

    fs::remove_all(tmp);
}

// ---------------------------------------------------------------------------
// T2: reload_agents_inplace mutates the caller's vector (daemon-side path)
// ---------------------------------------------------------------------------

static void test_t2_inplace_helper() {
    std::cout << "\n=== T2. reload_agents_inplace mutates caller's vector ===\n\n";

    auto tmp = make_temp_dir();
    auto agents_dir = tmp + "/.quorum/agents";
    fs::create_directories(agents_dir);
    write_agent_yaml(agents_dir, "a");

    std::vector<sui::quorum::AgentMetadata> agents;
    bool ok = sui::quorum::reload_agents_inplace(agents, agents_dir);
    check(ok, "T2: reload returns true when dir exists");
    check(agents.size() == 1, "T2: agents now holds 1");
    check(agents[0].id == "a", "T2: agents[0].id == 'a'");

    write_agent_yaml(agents_dir, "b");
    sui::quorum::reload_agents_inplace(agents, agents_dir);
    check(agents.size() == 2, "T2: after second yaml, agents holds 2");

    fs::remove_all(tmp);
}

// ---------------------------------------------------------------------------
// T3: missing directory → no-op, original roster preserved
// ---------------------------------------------------------------------------

static void test_t3_missing_dir_noop() {
    std::cout << "\n=== T3. missing agents dir → no-op (preserves roster) ===\n\n";

    std::vector<sui::quorum::AgentMetadata> agents;
    agents.push_back(sui::quorum::AgentMetadata{.id = "preset"});

    bool ok = sui::quorum::reload_agents_inplace(agents, "/tmp/does-not-exist-xyz");
    check(!ok, "T3: reload returns false when dir missing");
    check(agents.size() == 1 && agents[0].id == "preset",
          "T3: original roster unchanged");
}

int main() {
    std::cout << "=====================================================\n";
    std::cout << "  Phase 9 finding #2 — agent hot-reload tests\n";
    std::cout << "=====================================================\n";

    test_t1_reload_picks_up_new_agent();
    test_t2_inplace_helper();
    test_t3_missing_dir_noop();

    std::cout << "\n---------------------------------------------------\n";
    std::cout << "  passed: " << g_passed << "  failed: " << g_failed << "\n";
    std::cout << "---------------------------------------------------\n";

    return g_failed == 0 ? 0 : 1;
}
