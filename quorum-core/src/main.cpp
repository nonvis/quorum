#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "utils/config.h"
#include "daemon/scheduler.h"
#include "daemon/message_bus.h"
#include "daemon/event_dispatcher.h"
#include "daemon/conversation.h"
#include "storage/database.h"
#include "storage/schema.h"
#include "agent/invoker.h"
#include "agent/context_assembler.h"
#include "agent/output_parser.h"
#include "vault/vault_manager.h"
#include "cli/agent_create.h"
#include "cli/agent_history.h"
#include "cli/benchmark.h"
#include "cli/init.h"
#include "cli/skills.h"
#include "cli/vault_dedup.h"
#include "cli/vault_audit.h"
#include "cli/ask.h"
#include "cli/search.h"
#include "cli/supervisor_init.h"
#include "cli/knower_refresh.h"
#include "cli/spend.h"
#include "utils/discover.h"
#include "utils/self_path.h"
#include "utils/version.h"
// Generated at BUILD time by src/version_stamp.cmake (build dir, never
// committed): QUORUM_VERSION / QUORUM_GIT_SHA / QUORUM_GIT_DIRTY /
// QUORUM_BUILD_UTC. The sha is baked in -- `quorum version` never runs git.
#include "quorum_version.h"

namespace fs = std::filesystem;

static std::atomic<bool> g_running{true};

static void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    g_running.store(false, std::memory_order_release);
}

static uint64_t epoch_seconds() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count());
}

static std::string format_ts(uint64_t ts) {
    std::time_t t = static_cast<std::time_t>(ts);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::gmtime(&t));
    return std::string(buf);
}

static bool acquire_pid_lock(const std::string& path) {
    std::ifstream in(path);
    if (in.is_open()) {
        pid_t existing_pid = 0;
        in >> existing_pid;
        in.close();
        if (existing_pid > 0 && kill(existing_pid, 0) == 0) {
            std::cerr << "ERROR: Another instance running (PID "
                      << existing_pid << ")" << std::endl;
            return false;
        }
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return false;
    out << getpid() << std::endl;
    return true;
}

static void release_pid_lock(const std::string& path) {
    std::remove(path.c_str());
}

static void print_conversations(sui::quorum::Database& db) {
    std::cout << "Conversations:\n";
    int count = 0;
    db.query(
        "SELECT id, goal, state, round, max_rounds, budget_usd, spent_usd, "
        "created_at, paused_reason, current_agent, team FROM conversations ORDER BY id DESC",
        [&](sqlite3_stmt* stmt) {
            auto id = sqlite3_column_int64(stmt, 0);
            auto goal_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            auto state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            auto round = sqlite3_column_int(stmt, 3);
            auto max_r = sqlite3_column_int(stmt, 4);
            auto budget = sqlite3_column_double(stmt, 5);
            auto spent = sqlite3_column_double(stmt, 6);
            auto reason_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
            auto current_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));

            std::string goal = goal_raw ? std::string(goal_raw) : "";
            std::string reason = reason_raw ? std::string(reason_raw) : "";
            std::string current = current_raw ? std::string(current_raw) : "";
            auto team_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
            std::string team = team_raw ? std::string(team_raw) : "";
            if (goal.size() > 50) goal = goal.substr(0, 50) + "...";

            std::cout << "  #" << id
                      << "  " << (state ? state : "?");
            if (!current.empty()) std::cout << " [" << current << "]";
            if (!team.empty()) std::cout << " {" << team << "}";
            std::cout << "  turn " << round << "/" << max_r
                      << "  $" << spent << "/$" << budget
                      << "  " << goal;
            if (!reason.empty()) std::cout << "  (" << reason << ")";
            std::cout << "\n";

            // Per-conversation task counts
            auto total = db.query_int(
                "SELECT COUNT(*) FROM tasks WHERE conversation_id = "
                + std::to_string(id));
            auto done = db.query_int(
                "SELECT COUNT(*) FROM tasks WHERE conversation_id = "
                + std::to_string(id) + " AND status = 'done'");
            std::cout << "    tasks: " << done << "/" << total << " done\n";

            ++count;
        }
    );
    if (count == 0) {
        std::cout << "  No conversations.\n";
    }
}

static void print_usage(const char* prog) {
    std::cerr << "Usage:\n"
              << "  " << prog << " init                                      Initialize .quorum/ in current directory\n"
              << "  " << prog << " converse \"goal text\"                      Run conversation to completion, then exit (auto-discovers .quorum/)\n"
              << "  " << prog << " converse --keep-alive \"goal text\"        Run conversation, then keep daemon running (persistent mode)\n"
              << "  " << prog << " skills                                     List available Claude Code skills\n"
              << "  " << prog << " vault dedup [--vault <path>] [--dry-run] [--global] [--threshold <f>]\n"
              << "                                          Cluster near-duplicate rule-*.md/ref-*.md files\n"
              << "  " << prog << " vault audit [--vault <path>] [--days N] [--global]\n"
              << "                                          List stale (last_reviewed > N days) and expired rule/ref files\n"
              << "  " << prog << " ask \"<question>\" [--project <path|name>] [--agent <name>]\n"
              << "                                          Ask a project's manager (or a specific --agent) a question, read-only\n"
              << "  " << prog << " search \"<query>\" [--project <path|name>] [--agent <name>] [--limit N] [--json]\n"
              << "                                          Deterministic (no-LLM) ranked keyword search over the project's ref-*.md knower notes\n"
              << "  " << prog << " knower refresh [--all [--parallel] | --knower <name>] [--project <path|name>]\n"
              << "                                          Re-run the read-only knower scan(s) so the knower vaults re-survey the codebase\n"
              << "                                          --parallel (with --all): refresh independent lenses concurrently (cartographer->architect stays ordered);\n"
              << "                                            output is buffered per lens. Opt-in — see docs/proposals/knower-refresh-scaling.md\n"
              << "  " << prog << " spend [--project <path|name>] [--since <ISO8601>] [--until <ISO8601>] [--json]\n"
              << "                                          Per-run token/$ spend readout from the Claude Code transcripts (deterministic, $0);\n"
              << "                                            --since defaults to the flight start in .quorum/autopilot/LOCK\n"
              << "  " << prog << " benchmark --role <r> --task <name>          Run one synthetic benchmark for a role-specialty\n"
              << "  " << prog << " benchmark --role <r>                        Run all benchmarks for a role-specialty (aggregate)\n"
              << "  " << prog << " benchmark --role <r> --dry-run              Smoke-test setup; skip the daemon spawn\n"
              << "  " << prog << " benchmark --role <r> --keep-tempdir         Skip tempdir cleanup; print path for inspection\n"
              << "  " << prog << " version                                   Print the build identity (version, git sha, build stamp); also --version\n"
              << "  " << prog << " status                                    List conversations\n"
              << "  " << prog << " --config <path>                            Start daemon\n"
              << "  " << prog << " --config <path> resume --conversation <id> Resume paused\n"
              << "  " << prog << " --config <path> close --conversation <id>  Close conversation\n"
              << "  " << prog << " --config <path> respond --conversation <id> \"text\"  Respond to human request\n"
              << "  " << prog << " --config <path> agent create --role <r> --name <n> --project <p>\n"
              << "  " << prog << " agent modify --name <id> --description \"new desc\"   Modify agent\n"
              << "  " << prog << " agent list                                            List all agents\n"
              << "  " << prog << " agent history --name <id>                             Show CONTEXT.md audit trail\n"
              << "\nOptions:\n"
              << "  --config <path>      Path to config YAML (optional if .quorum/ exists in project)\n"
              << "  --verbose            Enable verbose logging\n"
              << "  --max-rounds <n>     Max revision rounds (default: 3)\n"
              << "  --mode <generic|brainstorm>  Conversation mode (default: generic)\n"
              << "  --keep-alive         converse only: keep the daemon running after the conversation completes (persistent mode)\n"
              << "  --once               converse only: exit when the conversation completes (now the default; retained for back-compat)\n"
              << "  --no-vault-write     Suppress VAULT_UPDATE filesystem writes for this conversation\n"
              << "  --ungated            converse only: opt a brainstorm out of the human-approval gate (single-knower scans); brainstorms gate by default\n"
              << "  --conversation <id>  Conversation ID for resume/close\n"
              << "  --help               Show this message\n"
              << "\nAgent create options:\n"
              << "  --role <role>        Agent role (leader|thinker|doer|evaluator)\n"
              << "  --name <name>        Agent ID\n"
              << "  --project <name>     Project subfolder in configs/agents/ (optional with .quorum/)\n"
              << "  --description <d>    Agent description (optional)\n"
              << "  --skill-file <path>  Path to SKILL.md (optional)\n"
              << "  --skill <name>       Shorthand for --skill-file .claude/skills/<name>/SKILL.md\n"
              << "  --target-dir <path>  Working directory for doer agents (optional)\n"
              << "  --no-ai              Skip AI generation, copy template as-is\n"
              << "  --regenerate         Regenerate CONTEXT.md without changing fields\n";
}

// The single line `quorum version` / `quorum --version` prints. Every fact is
// baked into the generated quorum_version.h at build time -- no runtime git.
static std::string build_identity_line() {
    return sui::quorum::format_version_line(QUORUM_VERSION, QUORUM_GIT_SHA,
                                            QUORUM_GIT_DIRTY != 0,
                                            QUORUM_BUILD_UTC);
}

// Check if a column exists in a table (used to guard ALTER TABLE migrations)
static bool column_exists(sui::quorum::Database& db, const std::string& table, const std::string& column) {
    bool found = false;
    db.query(
        "SELECT 1 FROM pragma_table_info('" + table + "') WHERE name = '" + column + "'",
        [&](sqlite3_stmt*) { found = true; }
    );
    return found;
}

// Check if a table exists in the database (used to guard CREATE TABLE migrations
// for tables introduced after the initial schema, on databases that pre-date
// them). Mirrors column_exists().
static bool table_exists(sui::quorum::Database& db, const std::string& table) {
    bool found = false;
    db.query(
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = '" + table + "'",
        [&](sqlite3_stmt*) { found = true; }
    );
    return found;
}

// Initialize all database tables + run migrations
static void init_schema(sui::quorum::Database& db) {
    sui::quorum::create_schema(db);

    // Migrations for databases created before these columns existed
    if (!column_exists(db, "tasks", "conversation_id"))
        db.execute("ALTER TABLE tasks ADD COLUMN conversation_id INTEGER REFERENCES conversations(id)");
    if (!column_exists(db, "tasks", "session_id"))
        db.execute("ALTER TABLE tasks ADD COLUMN session_id TEXT");
    if (!column_exists(db, "conversations", "current_agent"))
        db.execute("ALTER TABLE conversations ADD COLUMN current_agent TEXT");
    if (!column_exists(db, "conversations", "path_index"))
        db.execute("ALTER TABLE conversations ADD COLUMN path_index INTEGER NOT NULL DEFAULT 0");
    if (!column_exists(db, "conversations", "team"))
        db.execute("ALTER TABLE conversations ADD COLUMN team TEXT");
    if (!column_exists(db, "conversations", "mode"))
        db.execute("ALTER TABLE conversations ADD COLUMN mode TEXT NOT NULL DEFAULT 'generic'");
    // Phase 10 Track 5 — --no-vault-write flag persistence (mirrors mode pattern)
    if (!column_exists(db, "conversations", "no_vault_write"))
        db.execute("ALTER TABLE conversations ADD COLUMN no_vault_write INTEGER NOT NULL DEFAULT 0");
    // Phase 14.1 — daemon-enforced brainstorm gate columns (mirrors mode pattern)
    if (!column_exists(db, "conversations", "gated"))
        db.execute("ALTER TABLE conversations ADD COLUMN gated INTEGER NOT NULL DEFAULT 0");
    if (!column_exists(db, "conversations", "gate_cleared"))
        db.execute("ALTER TABLE conversations ADD COLUMN gate_cleared INTEGER NOT NULL DEFAULT 0");

    // Phase 7 Track 5 — system-prompt split + cache metrics
    if (!column_exists(db, "tasks", "system_prompt"))
        db.execute("ALTER TABLE tasks ADD COLUMN system_prompt TEXT");
    if (!column_exists(db, "tasks", "cache_creation_input_tokens"))
        db.execute("ALTER TABLE tasks ADD COLUMN cache_creation_input_tokens INTEGER");
    if (!column_exists(db, "tasks", "cache_read_input_tokens"))
        db.execute("ALTER TABLE tasks ADD COLUMN cache_read_input_tokens INTEGER");

    // A4 — tasks.summary: the daemon's one-line verdict, written by
    // Invoker::mark_done. create_schema() carries it for fresh DBs; this
    // guarded ALTER covers every DB created before A4. Idempotent on re-run.
    if (!column_exists(db, "tasks", "summary"))
        db.execute("ALTER TABLE tasks ADD COLUMN summary TEXT");

    // Phase 8 Track 3 — evaluations table for evaluator archetype scores.
    // create_schema() above already runs CREATE TABLE IF NOT EXISTS, so this
    // block is normally a no-op. Kept as an explicit migration marker for old
    // DBs and parity with the column_exists pattern used by Phase 5/7.
    if (!table_exists(db, "evaluations")) {
        db.execute(
            "CREATE TABLE evaluations ("
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
        db.execute("CREATE INDEX idx_evaluations_conv "
                   "ON evaluations(conversation_id)");
        db.execute("CREATE INDEX idx_evaluations_scored "
                   "ON evaluations(scored_agent_id)");
    }

    // Phase 14.1c (FIX A) — pending_vault_updates table. create_schema() above
    // already runs CREATE TABLE IF NOT EXISTS, so this is normally a no-op;
    // kept as an explicit migration marker for old DBs (mirrors evaluations).
    if (!table_exists(db, "pending_vault_updates")) {
        db.execute(
            "CREATE TABLE pending_vault_updates ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  conversation_id INTEGER NOT NULL,"
            "  agent_id TEXT NOT NULL,"
            "  role TEXT NOT NULL,"
            "  mode TEXT NOT NULL,"
            "  path TEXT NOT NULL,"
            "  content TEXT NOT NULL,"
            "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
            ")"
        );
        db.execute("CREATE INDEX idx_pending_vault_updates_conv "
                   "ON pending_vault_updates(conversation_id)");
    }
}

// Count currently active (running) tasks
static int64_t count_active_tasks(sui::quorum::Database& db) {
    return db.query_int("SELECT COUNT(*) FROM tasks WHERE status = 'active'");
}

// Recover from daemon crash: clean up stale active tasks and re-dispatch affected conversations
static int recover_stale_tasks(sui::quorum::Database& db,
                                sui::quorum::ConversationEngine& engine,
                                bool verbose) {
    // Find all tasks that were active (in-flight) when daemon last died
    std::vector<std::pair<int64_t, int64_t>> stale; // {task_id, conv_id}
    db.query(
        "SELECT id, conversation_id FROM tasks WHERE status = 'active'",
        [&](sqlite3_stmt* stmt) {
            auto tid = sqlite3_column_int64(stmt, 0);
            auto cid = sqlite3_column_type(stmt, 1) != SQLITE_NULL
                       ? sqlite3_column_int64(stmt, 1) : 0;
            stale.push_back({tid, cid});
        }
    );

    if (stale.empty()) return 0;

    std::cout << "[recovery] found " << stale.size()
              << " stale active task(s) from previous run\n";

    // Mark stale tasks as failed
    for (const auto& [task_id, conv_id] : stale) {
        db.execute(
            "UPDATE tasks SET status = 'failed', "
            "error = 'interrupted by daemon restart', "
            "completed_at = datetime('now') WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, task_id);
            }
        );
        if (verbose) {
            std::cout << "[recovery] task " << task_id << " marked failed";
            if (conv_id > 0) std::cout << " (conversation " << conv_id << ")";
            std::cout << "\n";
        }
    }

    // Collect unique conversation IDs that were affected
    std::set<int64_t> affected_convs;
    for (const auto& [_, conv_id] : stale) {
        if (conv_id > 0) affected_convs.insert(conv_id);
    }

    // Re-dispatch affected active conversations to leader
    for (auto conv_id : affected_convs) {
        engine.recover(conv_id);
    }

    return static_cast<int>(stale.size());
}

// Get hourly cost
static double hourly_cost(sui::quorum::Database& db) {
    return db.query_double(
        "SELECT COALESCE(SUM(cost), 0.0) FROM tasks "
        "WHERE created_at > datetime('now', '-1 hour')"
    );
}

struct BudgetWindow {
    double budget_usd = 100.0;
    double window_hours = 5.0;
    std::string window_start;
    double spent_usd = 0.0;

    double remaining_usd() const { return budget_usd - spent_usd; }
};

static BudgetWindow get_budget_window(sui::quorum::Database& db) {
    BudgetWindow w;
    db.query(
        "SELECT budget_usd, window_hours, window_start, spent_usd "
        "FROM budget_window WHERE id = 1",
        [&](sqlite3_stmt* stmt) {
            w.budget_usd = sqlite3_column_double(stmt, 0);
            w.window_hours = sqlite3_column_double(stmt, 1);
            auto ws = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            if (ws) w.window_start = ws;
            w.spent_usd = sqlite3_column_double(stmt, 3);
        }
    );
    return w;
}

static bool is_window_expired(sui::quorum::Database& db) {
    // Use SQLite datetime arithmetic for reliable comparison
    return db.query_int(
        "SELECT CASE WHEN datetime(window_start, '+' || "
        "CAST(CAST(window_hours * 60 AS INTEGER) AS TEXT) || ' minutes') "
        "<= datetime('now') THEN 1 ELSE 0 END "
        "FROM budget_window WHERE id = 1"
    ) == 1;
}

static void reset_budget_window(sui::quorum::Database& db,
                                  double budget, double hours) {
    db.execute(
        "UPDATE budget_window SET budget_usd = ?, window_hours = ?, "
        "window_start = datetime('now'), spent_usd = 0.0 WHERE id = 1",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_double(stmt, 1, budget);
            sqlite3_bind_double(stmt, 2, hours);
        }
    );
}

// Claim the next pending task: atomically set status to active.
// Returns task_id or 0 if none available.
static int64_t claim_next_task(sui::quorum::Database& db) {
    int64_t task_id = 0;
    db.query(
        "SELECT id FROM tasks WHERE status = 'pending' ORDER BY id ASC LIMIT 1",
        [&](sqlite3_stmt* stmt) {
            task_id = sqlite3_column_int64(stmt, 0);
        }
    );
    if (task_id == 0) return 0;

    db.execute(
        "UPDATE tasks SET status = 'active', started_at = datetime('now') WHERE id = ? AND status = 'pending'",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, task_id);
        }
    );
    return task_id;
}

// Look up the agent name for a given task id.
// Returns empty string if the task doesn't exist.
static std::string get_task_agent(sui::quorum::Database& db, int64_t task_id) {
    std::string agent;
    db.query(
        "SELECT agent FROM tasks WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, task_id);
        },
        [&](sqlite3_stmt* stmt) {
            const char* text = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt, 0));
            if (text) agent = text;
        }
    );
    return agent;
}

int main(int argc, char* argv[]) {
    // Disable stdout buffering for real-time log tailing when redirected to file
    std::setbuf(stdout, nullptr);

    // Phase 1: extract global flags from anywhere in argv
    std::string config_path;
    bool verbose = false;
    std::vector<bool> consumed(argc, false);
    consumed[0] = true; // program name

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
            consumed[i - 1] = true;
            consumed[i] = true;
        } else if (arg == "--verbose") {
            verbose = true;
            consumed[i] = true;
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--version") {
            // Alias of the `version` subcommand; answered here so it works
            // with no .quorum/, no config and no subcommand at all.
            std::cout << build_identity_line() << "\n";
            return 0;
        }
    }

    bool explicit_config = !config_path.empty();

    // Phase 2: collect unconsumed args -> subcommand + subcommand args
    std::string subcommand;
    std::vector<std::string> sub_args;
    for (int i = 1; i < argc; ++i) {
        if (consumed[i]) continue;
        if (subcommand.empty()) {
            subcommand = argv[i];
        } else {
            sub_args.push_back(argv[i]);
        }
    }

    // Parse subcommand-specific flags
    int conv_max_rounds = -1;         // sentinel — will use config default
    int64_t conv_id_arg = 0;
    std::string goal_text;
    std::string response_text;
    std::string agent_subcmd;
    sui::quorum::cli::AgentCreateParams agent_params;
    std::string mode_name;
    std::string bench_role;
    std::string bench_task;
    bool bench_dry_run = false;
    bool bench_keep_tempdir = false;
    bool exit_on_complete = false;
    bool keep_alive = false;            // converse opt-out: stay persistent after completion
    bool conv_no_vault_write = false;   // Phase 10 Track 5
    int conv_gated = -1;                 // Phase 14.1: -1 auto, 0 force ungated
    sui::quorum::cli::VaultDedupOptions vault_dedup_opts;
    sui::quorum::cli::VaultAuditOptions vault_audit_opts;
    std::string vault_subcmd_arg;
    bool vault_path_explicit = false;
    sui::quorum::cli::AskOptions ask_opts;  // Phase 12 — `quorum ask`
    sui::quorum::cli::SearchOptions search_opts;  // Phase 15 — `quorum search`
    sui::quorum::cli::SupervisorInitOptions supervisor_init_opts;  // Phase 13
    std::string supervisor_subcmd_arg;
    sui::quorum::cli::KnowerRefreshOptions knower_refresh_opts;    // Phase 14 T3
    std::string knower_subcmd_arg;
    sui::quorum::cli::SpendOptions spend_opts;                     // per-run spend readout

    if (subcommand == "converse") {
        for (size_t i = 0; i < sub_args.size(); ++i) {
            if (sub_args[i] == "--max-rounds" && i + 1 < sub_args.size()) {
                conv_max_rounds = std::stoi(sub_args[++i]);
            } else if (sub_args[i] == "--mode" && i + 1 < sub_args.size()) {
                mode_name = sub_args[++i];
                if (mode_name != "generic" && mode_name != "brainstorm") {
                    std::cerr << "ERROR: --mode must be 'generic' or 'brainstorm' (got '"
                              << mode_name << "')\n";
                    return 1;
                }
            } else if (sub_args[i] == "--once") {
                exit_on_complete = true;
            } else if (sub_args[i] == "--keep-alive") {
                keep_alive = true;
            } else if (sub_args[i] == "--no-vault-write") {
                conv_no_vault_write = true;
            } else if (sub_args[i] == "--ungated") {
                // Phase 14.1 — force gated=0. Single-knower scans
                // (run-knower.sh) legitimately write without a human; the
                // interactive multi-lens brainstorm gates by default.
                conv_gated = 0;
            } else {
                goal_text = sub_args[i]; // last positional = goal
            }
        }
    } else if (subcommand == "resume" || subcommand == "close" || subcommand == "respond") {
        for (size_t i = 0; i < sub_args.size(); ++i) {
            if (sub_args[i] == "--conversation" && i + 1 < sub_args.size()) {
                conv_id_arg = std::stoll(sub_args[++i]);
            } else if (subcommand == "respond") {
                response_text = sub_args[i];
            }
        }
    } else if (subcommand == "agent") {
        for (size_t i = 0; i < sub_args.size(); ++i) {
            if (agent_subcmd.empty() && (sub_args[i] == "create" || sub_args[i] == "modify" || sub_args[i] == "list" || sub_args[i] == "history")) {
                agent_subcmd = sub_args[i];
            } else if (sub_args[i] == "--role" && i + 1 < sub_args.size()) {
                agent_params.role = sub_args[++i];
            } else if (sub_args[i] == "--name" && i + 1 < sub_args.size()) {
                agent_params.name = sub_args[++i];
            } else if (sub_args[i] == "--project" && i + 1 < sub_args.size()) {
                agent_params.project = sub_args[++i];
            } else if (sub_args[i] == "--description" && i + 1 < sub_args.size()) {
                agent_params.description = sub_args[++i];
            } else if (sub_args[i] == "--skill-file" && i + 1 < sub_args.size()) {
                agent_params.skill_file = sub_args[++i];
            } else if (sub_args[i] == "--skill" && i + 1 < sub_args.size()) {
                auto skill_name = sub_args[++i];
                agent_params.skill_file = ".claude/skills/" + skill_name + "/SKILL.md";
            } else if (sub_args[i] == "--target-dir" && i + 1 < sub_args.size()) {
                agent_params.target_dir = sub_args[++i];
            } else if (sub_args[i] == "--no-ai") {
                agent_params.no_ai = true;
            } else if (sub_args[i] == "--regenerate") {
                agent_params.regenerate = true;
            }
        }
    } else if (subcommand == "init") {
        // No additional flags needed
    } else if (subcommand == "skills") {
        // No additional flags needed
    } else if (subcommand == "benchmark") {
        for (size_t i = 0; i < sub_args.size(); ++i) {
            if (sub_args[i] == "--role" && i + 1 < sub_args.size()) {
                bench_role = sub_args[++i];
            } else if (sub_args[i] == "--task" && i + 1 < sub_args.size()) {
                bench_task = sub_args[++i];
            } else if (sub_args[i] == "--dry-run") {
                bench_dry_run = true;
            } else if (sub_args[i] == "--keep-tempdir") {
                bench_keep_tempdir = true;
            }
        }
    } else if (subcommand == "vault") {
        for (size_t i = 0; i < sub_args.size(); ++i) {
            if (vault_subcmd_arg.empty() &&
                (sub_args[i] == "dedup" || sub_args[i] == "audit")) {
                vault_subcmd_arg = sub_args[i];
            } else if (sub_args[i] == "--vault" && i + 1 < sub_args.size()) {
                auto v = sub_args[++i];
                vault_dedup_opts.vault_path = v;
                vault_audit_opts.vault_path = v;
                vault_path_explicit = true;
            } else if (sub_args[i] == "--dry-run") {
                vault_dedup_opts.dry_run = true;
            } else if (sub_args[i] == "--global") {
                vault_dedup_opts.use_global = true;
                vault_audit_opts.use_global = true;
            } else if (sub_args[i] == "--threshold" && i + 1 < sub_args.size()) {
                try {
                    vault_dedup_opts.threshold = std::stod(sub_args[++i]);
                } catch (...) {
                    std::cerr << "ERROR: --threshold requires a numeric value\n";
                    return 1;
                }
            } else if (sub_args[i] == "--days" && i + 1 < sub_args.size()) {
                try {
                    vault_audit_opts.days = std::stoi(sub_args[++i]);
                } catch (...) {
                    std::cerr << "ERROR: --days requires an integer value\n";
                    return 1;
                }
                if (vault_audit_opts.days < 0) {
                    std::cerr << "ERROR: --days must be non-negative\n";
                    return 1;
                }
            }
        }
        if (vault_subcmd_arg.empty()) {
            std::cerr << "ERROR: vault requires a sub-subcommand (dedup|audit)\n";
            std::cerr << "Usage: quorum vault dedup [--vault <path>] [--dry-run] [--global] [--threshold <f>]\n";
            std::cerr << "       quorum vault audit [--vault <path>] [--global] [--days N]\n";
            return 1;
        }
    } else if (subcommand == "ask") {
        // Phase 12 — `quorum ask "<question>" [--project <path|name>]
        //             [--agent <name>]`.
        // The question is positional (everything that isn't a flag);
        // --project and --agent each take the next arg.
        for (size_t i = 0; i < sub_args.size(); ++i) {
            if (sub_args[i] == "--project" && i + 1 < sub_args.size()) {
                ask_opts.project = sub_args[++i];
            } else if (sub_args[i] == "--agent" && i + 1 < sub_args.size()) {
                ask_opts.agent = sub_args[++i];
            } else {
                ask_opts.question = sub_args[i];  // last positional = question
            }
        }
    } else if (subcommand == "search") {
        // Phase 15 — `quorum search "<query>" [--project <path|name>]
        //             [--agent <name>] [--limit N] [--json]`.
        // The query is positional (everything that isn't a flag); --project and
        // --agent each take the next arg; --limit takes an int; --json is a bool.
        for (size_t i = 0; i < sub_args.size(); ++i) {
            if (sub_args[i] == "--project" && i + 1 < sub_args.size()) {
                search_opts.project = sub_args[++i];
            } else if (sub_args[i] == "--agent" && i + 1 < sub_args.size()) {
                search_opts.agent = sub_args[++i];
            } else if (sub_args[i] == "--limit" && i + 1 < sub_args.size()) {
                try {
                    search_opts.limit = std::stoi(sub_args[++i]);
                } catch (...) {
                    std::cerr << "ERROR: --limit requires an integer value\n";
                    return 1;
                }
            } else if (sub_args[i] == "--json") {
                search_opts.json = true;
            } else {
                search_opts.query = sub_args[i];  // last positional = query
            }
        }
    } else if (subcommand == "supervisor") {
        // Phase 13 — `quorum supervisor init [--project <path>] [--force]`.
        for (size_t i = 0; i < sub_args.size(); ++i) {
            if (supervisor_subcmd_arg.empty() && sub_args[i] == "init") {
                supervisor_subcmd_arg = sub_args[i];
            } else if (sub_args[i] == "--project" && i + 1 < sub_args.size()) {
                supervisor_init_opts.project_path = sub_args[++i];
            } else if (sub_args[i] == "--force") {
                supervisor_init_opts.force = true;
            }
        }
        if (supervisor_subcmd_arg.empty()) {
            std::cerr << "ERROR: supervisor requires a sub-subcommand (init)\n";
            std::cerr << "Usage: quorum supervisor init [--project <path>] [--force]\n";
            return 1;
        }
    } else if (subcommand == "knower") {
        // Phase 14 Track 3 — `quorum knower refresh [--knower <name>] [--all]
        //                     [--project <path|name>]`.
        for (size_t i = 0; i < sub_args.size(); ++i) {
            if (knower_subcmd_arg.empty() && sub_args[i] == "refresh") {
                knower_subcmd_arg = sub_args[i];
            } else if (sub_args[i] == "--knower" && i + 1 < sub_args.size()) {
                knower_refresh_opts.knower = sub_args[++i];
            } else if (sub_args[i] == "--all") {
                knower_refresh_opts.all = true;
            } else if (sub_args[i] == "--parallel") {
                knower_refresh_opts.parallel = true;
            } else if (sub_args[i] == "--project" && i + 1 < sub_args.size()) {
                knower_refresh_opts.project = sub_args[++i];
            }
        }
        if (knower_subcmd_arg.empty()) {
            std::cerr << "ERROR: knower requires a sub-subcommand (refresh)\n";
            std::cerr << "Usage: quorum knower refresh [--all [--parallel] | --knower <"
                      << sui::quorum::cli::knower_refresh_detail::valid_knowers_list()
                      << ">] [--project <path|name>]\n";
            return 1;
        }
    } else if (subcommand == "spend") {
        // Per-run token/$ spend readout — `quorum spend [--project <path|name>]
        //   [--since <ISO8601>] [--until <ISO8601>] [--json]`.
        for (size_t i = 0; i < sub_args.size(); ++i) {
            if (sub_args[i] == "--project" && i + 1 < sub_args.size()) {
                spend_opts.project = sub_args[++i];
            } else if (sub_args[i] == "--since" && i + 1 < sub_args.size()) {
                spend_opts.since = sub_args[++i];
            } else if (sub_args[i] == "--until" && i + 1 < sub_args.size()) {
                spend_opts.until = sub_args[++i];
            } else if (sub_args[i] == "--json") {
                spend_opts.json = true;
            }
        }
    } else if (subcommand == "version") {
        // No flags. Dispatched below, before any .quorum/ discovery or config
        // load -- `quorum version` must answer outside a project.
    } else if (!subcommand.empty() && subcommand != "status") {
        std::cerr << "Unknown subcommand: " << subcommand << "\n";
        print_usage(argv[0]);
        return 1;
    }

    // `quorum version` — the binary's identity. Config-free like `search`, and
    // deliberately the first thing dispatched: no .quorum/, no DB, no git.
    if (subcommand == "version") {
        std::cout << build_identity_line() << "\n";
        return 0;
    }

    // Init doesn't need --config -- it creates the config.
    // Compute the repo root from the binary path so init can resolve the
    // shipped knower SKILL templates: <repo>/build/quorum_daemon → parent
    // twice = <repo>. Uses the OS's real executable path (robust to the PATH
    // `quorum` symlink, where argv[0] is the bare name "quorum"); falls back to
    // fs::canonical(argv[0]) on odd platforms. Best-effort: empty string on
    // failure (init then falls back to $HOME/.claude/skills + the CWD ladder,
    // and never fails).
    if (subcommand == "init") {
        std::string quorum_root =
            sui::quorum::quorum_repo_root_from_exe(argv[0]);
        return sui::quorum::cli::init_project(quorum_root);
    }

    // Benchmark doesn't need --config -- it spawns a child daemon against a
    // fresh temp project. Set QUORUM_DAEMON_PATH so the spawned child
    // resolves to *this* binary (otherwise PATH lookup would miss
    // build/quorum_daemon). Use the OS's real executable path (robust to the
    // PATH `quorum` symlink, where argv[0] is the bare name "quorum"); fall
    // back to fs::canonical(argv[0]) on odd platforms.
    if (subcommand == "benchmark") {
        if (!std::getenv("QUORUM_DAEMON_PATH")) {
            std::string self = sui::quorum::self_executable_path();
            if (self.empty()) {
                try {
                    self = fs::canonical(fs::path(argv[0])).string();
                } catch (...) {
                    // best-effort; benchmark.h falls back to PATH lookup
                }
            }
            if (!self.empty()) {
                setenv("QUORUM_DAEMON_PATH", self.c_str(), 1);
            }
        }
        return sui::quorum::cli::run_benchmark(
            bench_role, bench_task, bench_dry_run, verbose, bench_keep_tempdir);
    }

    // Skills doesn't need --config -- reads .claude/skills/ directly
    if (subcommand == "skills") {
        auto root = sui::quorum::discover_project_root();
        if (!root) {
            std::cerr << "No .quorum/ found. Run 'quorum init' first.\n";
            return 1;
        }
        return sui::quorum::cli::list_skills(*root);
    }

    // Vault dedup/audit don't need --config unless --global is set.
    // Without --global: scan project-scope <project_root>/.quorum/knowledge/.
    // With --global: load config to read global_knowledge_path (handled below).
    if (subcommand == "vault" && vault_subcmd_arg == "dedup") {
        if (vault_dedup_opts.use_global && vault_path_explicit) {
            std::cerr << "ERROR: --global and --vault are mutually exclusive\n";
            return 1;
        }
        if (!vault_dedup_opts.use_global && !vault_path_explicit) {
            // Default: project-scope vault.
            auto root = sui::quorum::discover_project_root();
            if (!root) {
                std::cerr << "ERROR: no .quorum/ found in current or parent directories\n";
                std::cerr << "Run 'quorum init' or pass --vault <path>\n";
                return 1;
            }
            vault_dedup_opts.vault_path =
                (fs::path(*root) / ".quorum" / "knowledge").string();
            return sui::quorum::cli::run_vault_dedup(vault_dedup_opts);
        }
        if (!vault_dedup_opts.use_global && vault_path_explicit) {
            // Explicit --vault path; standalone.
            return sui::quorum::cli::run_vault_dedup(vault_dedup_opts);
        }
        // --global case falls through to config load below.
    }
    if (subcommand == "vault" && vault_subcmd_arg == "audit") {
        if (vault_audit_opts.use_global && vault_path_explicit) {
            std::cerr << "ERROR: --global and --vault are mutually exclusive\n";
            return 1;
        }
        if (!vault_audit_opts.use_global && !vault_path_explicit) {
            auto root = sui::quorum::discover_project_root();
            if (!root) {
                std::cerr << "ERROR: no .quorum/ found in current or parent directories\n";
                std::cerr << "Run 'quorum init' or pass --vault <path>\n";
                return 1;
            }
            vault_audit_opts.vault_path =
                (fs::path(*root) / ".quorum" / "knowledge").string();
            return sui::quorum::cli::run_vault_audit(vault_audit_opts);
        }
        if (!vault_audit_opts.use_global && vault_path_explicit) {
            return sui::quorum::cli::run_vault_audit(vault_audit_opts);
        }
        // --global case falls through to config load below.
    }

    // Phase 12 — `quorum ask "<question>" [--project <path|name>]`. No --config
    // needed: read-only single-shot leader invocation against the target project
    // (default cwd, or --project <path|name>). run_ask resolves the project
    // root (verifies .quorum/ exists), assembles the manager prompt, and prints
    // the synthesized answer. cwd = project root during the live call so the
    // leader can read the live code.
    if (subcommand == "ask") {
        return sui::quorum::cli::run_ask(ask_opts);
    }

    // Phase 15 — `quorum search "<query>" [--project <path|name>] [--agent
    // <name>] [--limit N] [--json]`. No --config / no DB: a deterministic,
    // no-LLM, read-only ranked keyword search over the project's accumulated
    // ref-*.md knower notes, reusing the daemon's own search_references()
    // scorer. run_search resolves the project root (default cwd, or --project
    // <path|name>), loads the ref corpus, and prints the ranked hits.
    if (subcommand == "search") {
        return sui::quorum::cli::run_search(search_opts);
    }

    // Phase 13 — `quorum supervisor init`. No --config needed: generates the
    // autopilot SUPERVISOR.md flight plan + scaffolds .quorum/autopilot/ against
    // the target project root (default discovered root, or --project <path>).
    if (subcommand == "supervisor" && supervisor_subcmd_arg == "init") {
        if (supervisor_init_opts.project_path.empty()) {
            auto root = sui::quorum::discover_project_root();
            if (root) supervisor_init_opts.project_path = *root;
            // else: run_supervisor_init defaults to cwd and reports if no .quorum/.
        }
        return sui::quorum::cli::run_supervisor_init(supervisor_init_opts);
    }

    // Phase 14 Track 3 — `quorum knower refresh`. No --config needed: re-runs the
    // read-only knower scan pass(es) against the target project (default cwd, or
    // --project <path|name>) by shelling out to scripts/run-knower.sh. Resolve the
    // quorum repo root from the OS's real executable path (the binary is
    // <repo>/build/quorum_daemon, and `make install` symlinks the CLI to it).
    // self_executable_path() is robust to the installed `quorum` name (where
    // argv[0] is the bare string "quorum"); falls back to fs::canonical(argv[0])
    // on odd platforms. Best-effort: empty on failure (see header).
    if (subcommand == "knower" && knower_subcmd_arg == "refresh") {
        knower_refresh_opts.quorum_root =
            sui::quorum::quorum_repo_root_from_exe(argv[0]);
        return sui::quorum::cli::run_knower_refresh(knower_refresh_opts);
    }

    // Per-run spend readout — `quorum spend`. No --config needed: shells out to
    // scripts/spend_readout.py (deterministic, read-only, $0). Resolve the quorum
    // repo root from the OS's real executable path (same self-resolution as
    // knower refresh, robust to the installed `quorum` PATH symlink).
    if (subcommand == "spend") {
        spend_opts.quorum_root =
            sui::quorum::quorum_repo_root_from_exe(argv[0]);
        return sui::quorum::cli::run_spend(spend_opts);
    }

    // Agent list/modify/history don't need --config -- they work with .quorum/ directly
    if (subcommand == "agent" && agent_subcmd == "list") {
        return sui::quorum::cli::list_agents();
    }
    if (subcommand == "agent" && agent_subcmd == "modify") {
        if (agent_params.name.empty()) {
            std::cerr << "ERROR: agent modify requires --name <agent_id>\n";
            return 1;
        }
        return sui::quorum::cli::modify_agent(agent_params);
    }
    if (subcommand == "agent" && agent_subcmd == "history") {
        return sui::quorum::cli::show_history(agent_params.name);
    }

    if (config_path.empty()) {
        auto discovered = sui::quorum::discover_config();
        if (discovered) {
            config_path = *discovered;
            if (verbose) {
                std::cout << "Auto-discovered config: " << config_path << "\n";
            }
        } else {
            std::cerr << "ERROR: no --config provided and no .quorum/ found in current or parent directories\n";
            std::cerr << "Run 'quorum init' to initialize a project, or use --config <path>\n";
            return 1;
        }
    }

    // If config was auto-discovered, chdir to project root so relative paths work
    if (!explicit_config) {
        auto project_root = sui::quorum::discover_project_root();
        if (project_root) {
            fs::current_path(*project_root);
            if (verbose) {
                std::cout << "Project root: " << *project_root << "\n";
            }
        }
    }

    // Load config
    auto cfg_opt = sui::quorum::load_config(config_path);
    if (!cfg_opt) {
        std::cerr << "ERROR: Failed to load config from " << config_path << "\n";
        return 1;
    }
    auto& cfg = *cfg_opt;

    sui::quorum::validate_config(cfg);

    // Vault dedup --global: now that config is loaded, resolve target path.
    if (subcommand == "vault" && vault_subcmd_arg == "dedup" && vault_dedup_opts.use_global) {
        if (cfg.global_knowledge_path.empty()) {
            std::cerr << "ERROR: --global requires 'global_knowledge_path' in .quorum/config.yaml "
                      << "(Track 2 feature; not yet configured)\n";
            return 2;
        }
        vault_dedup_opts.vault_path = cfg.global_knowledge_path;
        return sui::quorum::cli::run_vault_dedup(vault_dedup_opts);
    }
    if (subcommand == "vault" && vault_subcmd_arg == "audit" && vault_audit_opts.use_global) {
        if (cfg.global_knowledge_path.empty()) {
            std::cerr << "ERROR: --global requires 'global_knowledge_path' in .quorum/config.yaml "
                      << "(Track 2 feature; not yet configured)\n";
            return 2;
        }
        vault_audit_opts.vault_path = cfg.global_knowledge_path;
        return sui::quorum::cli::run_vault_audit(vault_audit_opts);
    }

    // ── Agent subcommand early exit (no DB, no daemon) ──────────────────
    if (subcommand == "agent") {
        if (agent_subcmd != "create") {
            std::cerr << "ERROR: unknown agent subcommand. Usage: agent create|modify|list|history\n";
            return 1;
        }
        if (agent_params.role.empty() || agent_params.name.empty()) {
            std::cerr << "ERROR: agent create requires --role and --name\n";
            return 1;
        }
        if (agent_params.project.empty() && !sui::quorum::discover_project_root()) {
            std::cerr << "ERROR: agent create requires --project (no .quorum/ found)\n";
            return 1;
        }
        agent_params.data_dir = cfg.daemon.data_dir;
        return sui::quorum::cli::create_agent(agent_params);
    }

    // Apply conversation defaults from config (sentinels -> config values)
    if (conv_max_rounds < 0) conv_max_rounds = cfg.conversations.default_max_rounds;

    // Ensure data directory exists
    fs::create_directories(cfg.daemon.data_dir);

    // Initialize database
    auto db_path = cfg.daemon.data_dir + "/quorum.db";
    sui::quorum::Database db(db_path);
    if (!db.is_open()) {
        std::cerr << "ERROR: Failed to open database: " << db_path << "\n";
        return 1;
    }
    init_schema(db);

    // Seed budget window from config defaults if no row exists
    {
        auto count = db.query_int("SELECT COUNT(*) FROM budget_window");
        if (count == 0) {
            db.execute(
                "INSERT INTO budget_window (id, budget_usd, window_hours, window_start, spent_usd) "
                "VALUES (1, ?, ?, datetime('now'), 0.0)",
                [&](sqlite3_stmt* stmt) {
                    sqlite3_bind_double(stmt, 1, cfg.budget.window_budget_usd);
                    sqlite3_bind_double(stmt, 2, cfg.budget.window_hours);
                }
            );
        }
    }

    // Context assembler — stateless, safe to construct early
    sui::quorum::ContextAssembler context_assembler;

    // Conversation engine — lightweight, needed for subcommands
    auto project_root_str = sui::quorum::discover_project_root();
    sui::quorum::ConversationEngine conversation_engine(
        db, cfg.conversations, cfg.agents, &context_assembler,
        project_root_str.value_or(""));

    // Recover from previous crash (if any)
    recover_stale_tasks(db, conversation_engine, verbose);

    // ── Subcommand early exits (no PID lock, no daemon) ──────────────────
    if (subcommand == "status") {
        print_conversations(db);
        return 0;
    }

    if (subcommand == "close") {
        if (conv_id_arg == 0) {
            std::cerr << "ERROR: close requires --conversation <id>\n";
            return 1;
        }
        auto conv = db.get_conversation(conv_id_arg);
        if (!conv) {
            std::cerr << "ERROR: conversation " << conv_id_arg << " not found\n";
            return 1;
        }
        conversation_engine.close(conv_id_arg);
        std::cout << "Conversation " << conv_id_arg << " closed.\n";
        return 0;
    }

    if (subcommand == "respond") {
        if (conv_id_arg == 0) {
            std::cerr << "ERROR: respond requires --conversation <id>\n";
            return 1;
        }
        if (response_text.empty()) {
            std::cerr << "ERROR: respond requires response text\n";
            return 1;
        }
        bool ok = conversation_engine.respond(conv_id_arg, response_text);
        if (!ok) {
            std::cerr << "ERROR: conversation " << conv_id_arg
                      << " cannot respond (not waiting_for_human?)\n";
            return 1;
        }
        std::cout << "Response sent to conversation " << conv_id_arg << ".\n";
        return 0;
    }

    // ── Banner + config summary (daemon paths only) ──────────────────────
    std::cout << "Quorum Daemon v0.2.0" << std::endl;
    std::cout << "====================" << std::endl;
    std::cout << "  Network:    " << cfg.chain.network << "\n";
    std::cout << "  Data dir:   " << cfg.daemon.data_dir << "\n";
    if (!cfg.daemon.target_dir.empty()) {
        std::cout << "  Target dir: " << cfg.daemon.target_dir << "\n";
    }
    std::cout << "  Log level:  " << cfg.daemon.log_level << "\n";
    std::cout << "  Agents:     " << cfg.agents.size() << "\n";
    for (const auto& a : cfg.agents) {
        std::cout << "    - " << a.id << " (" << a.agent_class << ")\n";
    }
    std::cout << "  Dispatch:   sequential (one task at a time)\n";
    std::cout << "  Conversations: "
              << (cfg.conversations.enabled ? "enabled" : "disabled")
              << " (max_turns: " << cfg.conversations.default_max_rounds
              << ")\n";
    {
        auto window = get_budget_window(db);
        std::cout << "  Budget:     $" << window.spent_usd << " / $"
                  << window.budget_usd << " (window: " << window.window_hours << "h)\n";
    }

    if (verbose) {
        std::cout << "  Database:   " << db_path << " (OK)\n";
    }

    // Signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // converse: exit-on-complete is the DEFAULT (run the conversation, then exit).
    // Operators opt out with --keep-alive for the old persistent behavior.
    // (--once still sets exit_on_complete above; it is now the default and retained for back-compat.)
    if (subcommand == "converse" && !keep_alive) {
        exit_on_complete = true;
    }

    // PID lock with graceful fallback for converse/resume
    bool needs_daemon = subcommand.empty() || subcommand == "converse" || subcommand == "resume";
    if (needs_daemon) {
        if (!acquire_pid_lock(cfg.daemon.pid_file)) {
            if (subcommand.empty()) {
                // No subcommand = operator wants to start daemon. Hard error.
                return 1;
            }
            // converse/resume: daemon already running, it will pick up our changes
            if (subcommand == "converse") {
                if (goal_text.empty()) {
                    std::cerr << "ERROR: converse requires a goal string\n";
                    return 1;
                }
                // A daemon is already running, so converse just seeds the conversation and
                // returns; the running daemon completes it. converse runs no loop here, so
                // exit-on-complete is moot in this branch.
                auto id = conversation_engine.start(goal_text, conv_max_rounds, mode_name, conv_no_vault_write, conv_gated);
                std::cout << "Conversation " << id << " created.\n";
                std::cout << "Daemon already running — it will pick up the conversation.\n";
            } else if (subcommand == "resume") {
                if (conv_id_arg == 0) {
                    std::cerr << "ERROR: resume requires --conversation <id>\n";
                    return 1;
                }
                bool ok = conversation_engine.resume(conv_id_arg);
                if (!ok) {
                    std::cerr << "ERROR: conversation " << conv_id_arg
                              << " cannot be resumed (not paused?)\n";
                    return 1;
                }
                std::cout << "Conversation " << conv_id_arg << " resumed.\n";
                std::cout << "Daemon already running — it will pick up the change.\n";
            }
            return 0;
        }
    }

    // Initialize subsystems
    sui::quorum::Scheduler scheduler;
    sui::quorum::MessageBus message_bus;
    sui::quorum::EventDispatcher events;
    sui::quorum::Invoker invoker(db);
    sui::quorum::OutputParser output_parser;
    sui::quorum::VaultManager vault_manager(cfg.daemon.data_dir);

    // Initialize vaults for all configured agents
    for (const auto& agent_meta : cfg.agents) {
        auto& agent_id = agent_meta.id;
        bool ok = vault_manager.init_vault(agent_id);
        if (verbose) {
            std::cout << "  Vault init: " << agent_id
                      << (ok ? " (OK)" : " (FAILED)") << "\n";
        }
    }

    // --once: target conversation id whose terminal state ends the daemon loop.
    // 0 = not in --once mode (daemon runs until SIGINT/SIGTERM as before).
    int64_t once_target_conv_id = 0;

    // converse/resume dispatch (daemon path — PID lock acquired)
    if (subcommand == "converse") {
        if (goal_text.empty()) {
            std::cerr << "ERROR: converse requires a goal string\n";
            release_pid_lock(cfg.daemon.pid_file);
            return 1;
        }
        auto id = conversation_engine.start(goal_text, conv_max_rounds, mode_name, conv_no_vault_write, conv_gated);
        std::cout << "Conversation " << id << " created. Starting daemon...\n";
        if (exit_on_complete) {
            once_target_conv_id = id;
            std::cout << "[--once] daemon will exit when conversation "
                      << id << " reaches a terminal state.\n";
        }
        // fall through to daemon loop
    } else if (subcommand == "resume") {
        if (conv_id_arg == 0) {
            std::cerr << "ERROR: resume requires --conversation <id>\n";
            release_pid_lock(cfg.daemon.pid_file);
            return 1;
        }
        bool ok = conversation_engine.resume(conv_id_arg);
        if (!ok) {
            std::cerr << "ERROR: conversation " << conv_id_arg
                      << " cannot be resumed (not paused?)\n";
            release_pid_lock(cfg.daemon.pid_file);
            return 1;
        }
        std::cout << "Conversation " << conv_id_arg << " resumed. Starting daemon...\n";
        // fall through to daemon loop
    }

    // Wire up event handlers
    events.on("daemon.started", [&](const std::string&) {
        if (verbose) std::cout << "[event] daemon.started\n";
    });

    events.on("scheduler.tick", [&](const std::string&) {
        message_bus.drain();
    });

    // Register heartbeat
    scheduler.add("heartbeat", 60, [&]() {
        auto active = count_active_tasks(db);
        auto window = get_budget_window(db);

        if (verbose) {
            std::cout << "[" << format_ts(epoch_seconds()) << "] heartbeat"
                      << " — active: " << active
                      << " pending_msgs: " << message_bus.pending()
                      << " window: $" << window.spent_usd << "/$" << window.budget_usd
                      << "\n";
        }
    });

    // Task dispatch: check for pending tasks and invoke them.
    // Handles both Task Queue tasks (operator-seeded) and
    // Conversation tasks (daemon-created via ConversationEngine).
    //
    // Phase 9 finding #2 — `last_dispatched_conv_id` lets us re-scan the
    // agents directory whenever a task from a different conversation enters
    // dispatch. This way the running daemon picks up agents added externally
    // via `quorum agent create` between conversations without restart, while
    // avoiding I/O on every same-conv task.
    int64_t last_dispatched_conv_id = 0;
    scheduler.add("task_dispatch", 5, [&]() {
        // Check window budget
        if (is_window_expired(db)) {
            reset_budget_window(db, cfg.budget.window_budget_usd, cfg.budget.window_hours);
            if (verbose) {
                std::cout << "[dispatch] budget window expired, resetting to $"
                          << cfg.budget.window_budget_usd << " / "
                          << cfg.budget.window_hours << "h\n";
            }
        }
        {
            auto window = get_budget_window(db);
            if (window.spent_usd >= window.budget_usd) {
                if (verbose) {
                    std::cout << "[dispatch] window budget exceeded ($"
                              << window.spent_usd << " >= $" << window.budget_usd
                              << "), pausing dispatch\n";
                }
                return;
            }
        }

        // Sequential dispatch: one task at a time
        auto active = count_active_tasks(db);
        if (active > 0) {
            return;  // sequential — wait for current task to complete
        }

        // Claim and dispatch
        auto task_id = claim_next_task(db);
        if (task_id == 0) return;  // no pending tasks

        if (verbose) {
            std::cout << "[dispatch] invoking task " << task_id << "\n";
        }

        // Phase 9 finding #2 — refresh agents from disk on conversation
        // boundary so externally-created agents become visible without
        // daemon restart.
        {
            auto conv_id_opt = db.get_conversation_for_task(task_id);
            int64_t conv_id = conv_id_opt.value_or(0);
            if (conv_id != 0 && conv_id != last_dispatched_conv_id) {
                sui::quorum::reload_agents_inplace(cfg.agents, ".quorum/agents");
                last_dispatched_conv_id = conv_id;
            }
        }

        // Look up agent metadata by id
        auto task_agent_name = get_task_agent(db, task_id);
        sui::quorum::AgentMetadata task_agent_meta;
        for (const auto& a : cfg.agents) {
            if (a.id == task_agent_name) {
                task_agent_meta = a;
                break;
            }
        }

        // Resolve conversation mode for this task (Phase 6 Track 2). If the
        // task belongs to a conversation, pull its mode from the DB so the
        // invoker can sandbox tools when mode == "brainstorm". Tasks not
        // attached to a conversation default to "generic".
        std::string task_mode = "generic";
        {
            auto conv_id_opt = db.get_conversation_for_task(task_id);
            if (conv_id_opt) {
                auto conv = db.get_conversation(*conv_id_opt);
                if (conv && !conv->mode.empty()) {
                    task_mode = conv->mode;
                }
            }
        }

        // Phase 10 Track 5 — resolve conversation-level VAULT_UPDATE suppression
        // flag. If set, the task_dispatch closure parses VAULT_UPDATE blocks
        // normally but skips the filesystem write step and emits a stderr/stdout
        // notice. The flag is set at conversation start() time and inherited by
        // resume/recover automatically.
        bool conv_no_vault_write = false;
        // Phase 14.1 — gated-brainstorm suppression inputs. In a GATED
        // brainstorm the daemon holds knower VAULT_UPDATE writes until a human
        // clears the gate (gate_cleared). This is the structural enforcement of
        // the L3 invariant — convention/SKILL text alone proved insufficient.
        bool conv_gated = false;
        bool conv_gate_cleared = false;
        {
            auto conv_id_opt = db.get_conversation_for_task(task_id);
            if (conv_id_opt) {
                auto conv = db.get_conversation(*conv_id_opt);
                if (conv) {
                    conv_no_vault_write = conv->no_vault_write;
                    conv_gated = conv->gated;
                    conv_gate_cleared = conv->gate_cleared;
                }
            }
        }

        auto result = invoker.invoke(task_id, task_agent_meta, task_mode);

        if (verbose) {
            if (result.success) {
                std::cout << "[dispatch] task " << task_id << " done"
                          << " — tokens_in: " << result.tokens_in
                          << " tokens_out: " << result.tokens_out
                          << " cost: $" << result.cost
                          << (result.session_id.empty() ? "" : " session: " + result.session_id.substr(0, 8))
                          << "\n";
            } else {
                std::cout << "[dispatch] task " << task_id
                          << " failed: " << result.error << "\n";
            }
        }

        // Track cost in budget window
        if (result.cost > 0) {
            db.execute(
                "UPDATE budget_window SET spent_usd = spent_usd + ? WHERE id = 1",
                [&](sqlite3_stmt* stmt) {
                    sqlite3_bind_double(stmt, 1, result.cost);
                }
            );
        }

        // Process structured output
        if (result.success && !result.output.empty()) {
            auto agent_id = get_task_agent(db, task_id);
            if (!agent_id.empty()) {
                auto parsed = output_parser.parse(result.output);

                // Resolve emitting agent's role for vault path classification.
                std::string emitting_role;
                for (const auto& a : cfg.agents) {
                    if (a.id == agent_id) {
                        emitting_role = a.role;
                        break;
                    }
                }

                // Apply vault updates with conversation context. The classifier
                // inside vault_manager enforces own-vault-only writes in BOTH
                // modes: a VAULT_UPDATE path must start with knowledge/ or
                // inbox/ and lands in the EMITTING agent's vault. (Phase 14:
                // knowers self-write their own lens's slice — own-vault — behind
                // the brainstorm human gate; there is no cross-vault curator.)
                if (!parsed.vault_updates.empty()) {
                    if (conv_no_vault_write) {
                        // Phase 10 Track 5 — suppression branch. Unconditional
                        // log (not verbose-gated) per spec #20 — operator must
                        // see when writes are being dropped.
                        std::cout << "[dispatch] task " << task_id
                                  << " — VAULT_UPDATE suppressed "
                                  << "(--no-vault-write, "
                                  << parsed.vault_updates.size()
                                  << " update(s) dropped)\n";
                    } else if (sui::quorum::brainstorm_gate_suppresses_write(
                                   task_mode, conv_gated, conv_gate_cleared)) {
                        // Phase 14.1c (FIX A) — daemon-enforced brainstorm gate.
                        // The knower (or a leader telling it to "write now")
                        // produced its VAULT_UPDATE before the human approved.
                        // Rather than DROP it (the old bug — the reviewed note
                        // was lost), STAGE each update; it flushes to the
                        // knower's own vault once the gate clears (gate_cleared).
                        // The L3 invariant is structurally guaranteed here, not
                        // by SKILL convention. Unconditional log so the operator
                        // sees the held write(s).
                        auto conv_id_for_stage =
                            db.get_conversation_for_task(task_id).value_or(0);
                        for (const auto& vu : parsed.vault_updates) {
                            db.stage_vault_update(conv_id_for_stage, agent_id,
                                                  emitting_role, task_mode,
                                                  vu.path, vu.content);
                        }
                        std::cout << "[conversation " << conv_id_for_stage
                                  << "] VAULT_UPDATE staged for human approval "
                                     "(gated brainstorm) — "
                                  << parsed.vault_updates.size()
                                  << " note(s) held\n";
                    } else {
                        auto applied = vault_manager.apply_all_updates_with_context(
                            agent_id, emitting_role, task_mode, cfg.agents,
                            parsed.vault_updates,
                            project_root_str.value_or(""));
                        if (verbose) {
                            std::cout << "[dispatch] task " << task_id
                                      << " — " << applied << "/" << parsed.vault_updates.size()
                                      << " vault updates applied for " << agent_id
                                      << " (mode=" << task_mode
                                      << ", role=" << emitting_role << ")\n";
                        }
                    }
                }

                // Phase 8 Track 3 — persist EVALUATION block to evaluations
                // table. Evaluator agent ID = agent_id (the agent whose turn
                // just produced the block). Scored agent ID is taken from
                // the optional `scored:` field; if absent, fall back to the
                // most recent task agent in this conversation other than the
                // evaluator. Empty fallback => persist with empty string.
                if (parsed.evaluation.has_value()) {
                    auto conv_id_opt =
                        db.get_conversation_for_task(task_id);
                    if (conv_id_opt) {
                        const auto& e = *parsed.evaluation;
                        std::string scored = e.scored;
                        if (scored.empty()) {
                            scored = db.previous_task_agent(
                                *conv_id_opt, agent_id);
                        }
                        auto eval_id = db.append_evaluation(
                            *conv_id_opt,
                            scored,
                            agent_id,
                            e.role_specialty,
                            e.rubric_version,
                            e.total_score,
                            e.items_json,
                            e.notes);
                        if (verbose) {
                            std::cout << "[dispatch] task " << task_id
                                      << " — evaluation #" << eval_id
                                      << " persisted ("
                                      << e.role_specialty
                                      << " v" << e.rubric_version
                                      << ", score=" << e.total_score
                                      << ", scored=" << scored
                                      << ")\n";
                        }
                    } else if (verbose) {
                        std::cout << "[dispatch] task " << task_id
                                  << " — EVALUATION block ignored (no "
                                  << "conversation context)\n";
                    }
                }

                // Log summary if present
                if (verbose && !parsed.summary.empty()) {
                    std::cout << "[dispatch] task " << task_id
                              << " summary: " << parsed.summary.substr(0, 200) << "\n";
                }
            }
        }

        // ── Conversation routing ──────────────────────────────────────────
        // If this task belongs to a conversation, route through the engine.
        // Runs AFTER existing processing so vault/consensus/observations
        // are handled for conversation tasks too.
        {
            auto conv_id_opt = db.get_conversation_for_task(task_id);
            if (conv_id_opt) {
                // Phase 14.1c (FIX A) — FLUSH staged knower writes once the
                // human has cleared a gated brainstorm. UNCONDITIONAL: this must
                // run even when the current task carried no VAULT_UPDATE (the
                // leader's response task that follows `respond "yes"` won't).
                // Idempotent — after clear, count is 0 on subsequent loops.
                {
                    auto flush_conv = db.get_conversation(*conv_id_opt);
                    if (flush_conv && flush_conv->mode == "brainstorm" &&
                        flush_conv->gated && flush_conv->gate_cleared &&
                        db.count_pending_vault_updates(*conv_id_opt) > 0) {
                        auto pending =
                            db.get_pending_vault_updates(*conv_id_opt);
                        for (const auto& pu : pending) {
                            sui::quorum::VaultUpdate vu{pu.path, pu.content};
                            auto applied =
                                vault_manager.apply_all_updates_with_context(
                                    pu.agent_id, pu.role, pu.mode, cfg.agents,
                                    {vu}, project_root_str.value_or(""));
                            (void)applied;
                        }
                        db.clear_pending_vault_updates(*conv_id_opt);
                        std::cout << "[conversation " << *conv_id_opt
                                  << "] flushed " << pending.size()
                                  << " approved knower write(s) to their vaults\n";
                    }
                }

                // Build ParsedOutput for the engine (may be empty if task failed)
                sui::quorum::ParsedOutput conv_parsed;
                if (result.success && !result.output.empty()) {
                    conv_parsed = output_parser.parse(result.output);
                }

                bool still_active = conversation_engine.on_task_complete(
                    task_id, conv_parsed, result.cost
                );
                (void)still_active;  // used by future dispatch logic

                if (verbose) {
                    auto conv = db.get_conversation(*conv_id_opt);
                    if (conv) {
                        std::cout << "[conversation " << conv->id << "] "
                                  << conv->state
                                  << " — " << conv->goal.substr(0, 60) << "\n";
                    }
                }
            }
        }

        // Emit event for result processing
        events.emit("task.completed", std::to_string(task_id));
    });

    events.emit("daemon.started");

    std::cout << "\n--- Daemon running (PID " << getpid() << ") ---\n";
    std::cout << "Press Ctrl+C to stop.\n" << std::endl;

    // Main loop
    while (g_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!g_running.load(std::memory_order_acquire)) break;

        uint64_t now = epoch_seconds();
        scheduler.tick(now);
        message_bus.drain();

        // --once: exit cleanly when the target conversation reaches terminal state.
        if (once_target_conv_id != 0) {
            auto conv = db.get_conversation(once_target_conv_id);
            if (conv && (conv->state == "done" || conv->state == "closed")) {
                std::cout << "\n[--once] conversation " << once_target_conv_id
                          << " reached terminal state '" << conv->state
                          << "' — exiting daemon loop.\n";
                break;
            }
        }
    }

    std::cout << "\nShutting down..." << std::endl;

    // Print final stats
    auto total_done = db.query_int("SELECT COUNT(*) FROM tasks WHERE status = 'done'");
    auto total_failed = db.query_int("SELECT COUNT(*) FROM tasks WHERE status = 'failed'");
    auto total_cost = hourly_cost(db);
    std::cout << "  Tasks completed: " << total_done << "\n";
    std::cout << "  Tasks failed:    " << total_failed << "\n";
    std::cout << "  Cost (1h):       $" << total_cost << "\n";

    release_pid_lock(cfg.daemon.pid_file);
    std::cout << "Shutdown complete." << std::endl;
    return 0;
}
