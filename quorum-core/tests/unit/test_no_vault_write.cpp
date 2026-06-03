// tests/unit/test_no_vault_write.cpp
// Phase 10 Track 5 #21 — `--no-vault-write` flag end-to-end.
//
// Three checks:
//   A. Suppression path: simulate a VAULT_UPDATE-bearing agent output when
//      conv.no_vault_write == true. Assert vault_manager is NOT called
//      and no file appears on disk.
//   B. Pass-through control: same input with no_vault_write == false.
//      Assert the file DOES appear on disk (proves the test isn't no-oping
//      for the wrong reason).
//   C. DB persistence: ConversationEngine::start(..., no_vault_write=true)
//      writes no_vault_write=1; default-arg form writes 0.
//
// Run:  cd build && ctest -R test_no_vault_write --output-on-failure

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <sqlite3.h>
#include <unistd.h>

#include "storage/database.h"
#include "daemon/conversation.h"
#include "agent/output_parser.h"
#include "vault/vault_manager.h"
#include "utils/config.h"

namespace fs = std::filesystem;

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

// Minimal schema mirror — only the columns ConversationEngine + the test touch.
// Same approach as test_generic_loop.cpp::init_schema().
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
        "  system_prompt TEXT,"
        "  cache_creation_input_tokens INTEGER,"
        "  cache_read_input_tokens INTEGER"
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
}

// Canonical scribe-emitting VAULT_UPDATE block (mirrors
// test_inventory_write_side.cpp::make_scribe_output).
static std::string make_scribe_output(const std::string& path,
                                      const std::string& body) {
    std::string out;
    out += "Update applied.\n\n";
    out += "```VAULT_UPDATE\n";
    out += "path: " + path + "\n";
    out += "content: |\n";
    size_t start = 0;
    while (start <= body.size()) {
        auto nl = body.find('\n', start);
        if (nl == std::string::npos) {
            out += "  " + body.substr(start) + "\n";
            break;
        }
        out += "  " + body.substr(start, nl - start) + "\n";
        start = nl + 1;
    }
    out += "```\n";
    return out;
}

// Replays the main.cpp task_dispatch VAULT_UPDATE branch under a no_vault_write
// boolean. This is the suppression-guard contract under test.
static size_t simulate_dispatch(sui::quorum::OutputParser& parser,
                                const sui::quorum::VaultManager& vm,
                                const std::vector<sui::quorum::AgentMetadata>& team,
                                const std::string& raw_output,
                                bool no_vault_write) {
    auto parsed = parser.parse(raw_output);
    if (parsed.vault_updates.empty()) return 0;
    if (no_vault_write) {
        // Mirrors the suppression branch in main.cpp.
        std::cout << "[dispatch] task X — VAULT_UPDATE suppressed "
                  << "(--no-vault-write, " << parsed.vault_updates.size()
                  << " update(s) dropped)\n";
        return 0;
    }
    return vm.apply_all_updates_with_context(
        /*emitting_agent_id=*/"scribe",
        /*emitting_agent_role=*/"scribe",
        /*conversation_mode=*/"generic",
        team,
        parsed.vault_updates);
}

// ---- Case A: suppression path ----------------------------------------------
static void test_A_suppression_blocks_writes(const fs::path& tdir) {
    std::cout << "\n=== Case A: suppression blocks writes ===\n\n";

    auto base = tdir / "caseA";
    fs::create_directories(base);

    sui::quorum::VaultManager vm(base.string());
    check(vm.init_vault("scribe"), "A: init_vault(scribe) succeeds");

    auto knowledge_dir = base / "vaults" / "scribe" / "knowledge";
    auto rule_path     = knowledge_dir / "rule-foo.md";

    std::vector<sui::quorum::AgentMetadata> team = {
        sui::quorum::AgentMetadata{
            .id = "scribe", .name = "Scribe",
            .description = "Curates", .role = "scribe"
        },
    };

    sui::quorum::OutputParser parser;
    auto out = make_scribe_output(
        "knowledge/rule-foo.md",
        "# rule-foo\n\nShould not land on disk.");

    auto applied = simulate_dispatch(parser, vm, team, out,
                                      /*no_vault_write=*/true);
    check(applied == 0, "A: zero updates applied when no_vault_write=true");
    check(!fs::exists(rule_path),
          "A: knowledge/rule-foo.md does NOT exist after suppressed dispatch");
}

// ---- Case B: pass-through control ------------------------------------------
static void test_B_passthrough_writes_when_flag_clear(const fs::path& tdir) {
    std::cout << "\n=== Case B: pass-through writes when flag clear ===\n\n";

    auto base = tdir / "caseB";
    fs::create_directories(base);

    sui::quorum::VaultManager vm(base.string());
    check(vm.init_vault("scribe"), "B: init_vault(scribe) succeeds");

    auto knowledge_dir = base / "vaults" / "scribe" / "knowledge";
    auto rule_path     = knowledge_dir / "rule-foo.md";

    std::vector<sui::quorum::AgentMetadata> team = {
        sui::quorum::AgentMetadata{
            .id = "scribe", .name = "Scribe",
            .description = "Curates", .role = "scribe"
        },
    };

    sui::quorum::OutputParser parser;
    auto out = make_scribe_output(
        "knowledge/rule-foo.md",
        "# rule-foo\n\nMust land on disk.");

    auto applied = simulate_dispatch(parser, vm, team, out,
                                      /*no_vault_write=*/false);
    check(applied == 1, "B: one update applied when no_vault_write=false");
    check(fs::exists(rule_path),
          "B: knowledge/rule-foo.md exists after normal dispatch");
}

// ---- Case C: DB persistence ------------------------------------------------
static void test_C_db_persistence() {
    std::cout << "\n=== Case C: DB persistence via start() ===\n\n";

    sui::quorum::Database db(":memory:");
    init_schema(db);

    sui::quorum::ConversationConfig cfg;
    cfg.leader = "leader";
    cfg.default_max_rounds = 20;
    cfg.default_budget_usd = 5.0;

    std::vector<sui::quorum::AgentMetadata> agents;
    agents.push_back(sui::quorum::AgentMetadata{.id = "leader"});

    sui::quorum::ConversationEngine engine(db, cfg, agents);

    // Start two conversations: one with flag clear, one set.
    auto id_clear = engine.start("goal-clear", 5.0, 20, "", false);
    auto id_set   = engine.start("goal-set",   5.0, 20, "", true);

    auto conv_clear = db.get_conversation(id_clear);
    auto conv_set   = db.get_conversation(id_set);

    check(conv_clear.has_value(), "C: conversation (clear) round-trips");
    check(conv_set.has_value(),   "C: conversation (set) round-trips");
    check(!conv_clear->no_vault_write,
          "C: no_vault_write == false for clear conversation");
    check(conv_set->no_vault_write,
          "C: no_vault_write == true for set conversation");
}

// ---- main ------------------------------------------------------------------
int main() {
    auto tdir = fs::temp_directory_path() /
                ("quorum-test-no-vault-write-" + std::to_string(::getpid()));
    fs::create_directories(tdir);

    test_A_suppression_blocks_writes(tdir);
    test_B_passthrough_writes_when_flag_clear(tdir);
    test_C_db_persistence();

    std::error_code ec;
    fs::remove_all(tdir, ec);

    std::cout << "\nResults: " << g_passed << " passed, " << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
