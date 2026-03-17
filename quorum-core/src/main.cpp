#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
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
#include "agent/invoker.h"
#include "agent/context_assembler.h"
#include "agent/output_parser.h"
#include "vault/vault_manager.h"

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
        "created_at, paused_reason, current_agent FROM conversations ORDER BY id DESC",
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
            if (goal.size() > 50) goal = goal.substr(0, 50) + "...";

            std::cout << "  #" << id
                      << "  " << (state ? state : "?");
            if (!current.empty()) std::cout << " [" << current << "]";
            std::cout << "  round " << round << "/" << max_r
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
              << "  " << prog << " --config <path>                            Start daemon\n"
              << "  " << prog << " --config <path> converse \"goal text\"       Start conversation + daemon\n"
              << "  " << prog << " --config <path> converse --budget 3.0 \"g\"  Custom budget\n"
              << "  " << prog << " --config <path> status                     List conversations\n"
              << "  " << prog << " --config <path> resume --conversation <id> Resume paused\n"
              << "  " << prog << " --config <path> close --conversation <id>  Close conversation\n"
              << "  " << prog << " --config <path> respond --conversation <id> \"text\"  Respond to human request\n"
              << "\nOptions:\n"
              << "  --config <path>      Path to config YAML (required, e.g. configs/mm-bot.yaml)\n"
              << "  --verbose            Enable verbose logging\n"
              << "  --budget <usd>       Per-conversation budget (default: 5.0)\n"
              << "  --max-rounds <n>     Max revision rounds (default: 3)\n"
              << "  --conversation <id>  Conversation ID for resume/close\n"
              << "  --help               Show this message\n";
}

// Initialize all database tables
static void init_schema(sui::quorum::Database& db) {
    db.execute(
        "CREATE TABLE IF NOT EXISTS audit_log ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp INTEGER NOT NULL,"
        "  agent TEXT NOT NULL,"
        "  action TEXT NOT NULL,"
        "  details TEXT"
        ")"
    );
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
        "  path_index INTEGER NOT NULL DEFAULT 0"
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
    db.execute("CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status)");
    db.execute("CREATE INDEX IF NOT EXISTS idx_tasks_agent ON tasks(agent)");
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
    db.execute(
        "CREATE TABLE IF NOT EXISTS agent_sessions ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  cycle_id    INTEGER NOT NULL REFERENCES conversations(id),"
        "  agent_id    TEXT NOT NULL,"
        "  session_id  TEXT NOT NULL,"
        "  UNIQUE(cycle_id, agent_id)"
        ")"
    );
    // Migration for existing databases
    db.execute("ALTER TABLE tasks ADD COLUMN conversation_id INTEGER REFERENCES conversations(id)");
    db.execute("ALTER TABLE tasks ADD COLUMN session_id TEXT");
    db.execute("ALTER TABLE conversations ADD COLUMN current_agent TEXT");
    db.execute("ALTER TABLE conversations ADD COLUMN path_index INTEGER NOT NULL DEFAULT 0");
}

// Count currently active (running) tasks
static int64_t count_active_tasks(sui::quorum::Database& db) {
    return db.query_int("SELECT COUNT(*) FROM tasks WHERE status = 'active'");
}

// Get hourly cost
static double hourly_cost(sui::quorum::Database& db) {
    return db.query_double(
        "SELECT COALESCE(SUM(cost), 0.0) FROM tasks "
        "WHERE created_at > datetime('now', '-1 hour')"
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

// Look up the task_type for a given task id.
// Returns empty string if the task doesn't exist.
static std::string get_task_type(sui::quorum::Database& db, int64_t task_id) {
    std::string task_type;
    db.query(
        "SELECT task_type FROM tasks WHERE id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, task_id);
        },
        [&](sqlite3_stmt* stmt) {
            const char* text = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt, 0));
            if (text) task_type = text;
        }
    );
    return task_type;
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
        }
    }

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
    double conv_budget = -1.0;        // sentinel — will use config default
    int conv_max_rounds = -1;         // sentinel — will use config default
    int64_t conv_id_arg = 0;
    std::string goal_text;
    std::string response_text;

    if (subcommand == "converse") {
        for (size_t i = 0; i < sub_args.size(); ++i) {
            if (sub_args[i] == "--budget" && i + 1 < sub_args.size()) {
                conv_budget = std::stod(sub_args[++i]);
            } else if (sub_args[i] == "--max-rounds" && i + 1 < sub_args.size()) {
                conv_max_rounds = std::stoi(sub_args[++i]);
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
    } else if (!subcommand.empty() && subcommand != "status") {
        std::cerr << "Unknown subcommand: " << subcommand << "\n";
        print_usage(argv[0]);
        return 1;
    }

    if (config_path.empty()) {
        std::cerr << "ERROR: --config is required\n";
        print_usage(argv[0]);
        return 1;
    }

    // Load config
    auto cfg_opt = sui::quorum::load_config(config_path);
    if (!cfg_opt) {
        std::cerr << "ERROR: Failed to load config from " << config_path << "\n";
        return 1;
    }
    auto& cfg = *cfg_opt;

    // Apply conversation defaults from config (sentinels -> config values)
    if (conv_budget < 0) conv_budget = cfg.conversations.default_budget_usd;
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

    // Conversation engine — lightweight, needed for subcommands
    sui::quorum::ConversationEngine conversation_engine(db, cfg.conversations, cfg.agents);

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
              << " (budget: $" << cfg.conversations.default_budget_usd
              << ", max_rounds: " << cfg.conversations.default_max_rounds
              << ")\n";

    if (verbose) {
        std::cout << "  Database:   " << db_path << " (OK)\n";
    }

    // Signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

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
                auto id = conversation_engine.start(goal_text, conv_budget, conv_max_rounds);
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
    sui::quorum::ContextAssembler context_assembler;
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

    // converse/resume dispatch (daemon path — PID lock acquired)
    if (subcommand == "converse") {
        if (goal_text.empty()) {
            std::cerr << "ERROR: converse requires a goal string\n";
            release_pid_lock(cfg.daemon.pid_file);
            return 1;
        }
        auto id = conversation_engine.start(goal_text, conv_budget, conv_max_rounds);
        std::cout << "Conversation " << id << " created. Starting daemon...\n";
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
        auto cost_h = hourly_cost(db);

        if (verbose) {
            std::cout << "[" << format_ts(epoch_seconds()) << "] heartbeat"
                      << " — active: " << active
                      << " pending_msgs: " << message_bus.pending()
                      << " cost_1h: $" << cost_h
                      << "\n";
        }
    });

    // Task dispatch: check for pending tasks and invoke them.
    // Handles both Task Queue tasks (operator-seeded) and
    // Conversation tasks (daemon-created via ConversationEngine).
    scheduler.add("task_dispatch", 5, [&]() {
        // Check budget
        auto cost_h = hourly_cost(db);
        if (cost_h >= cfg.budget.hourly_limit_usd) {
            if (verbose) {
                std::cout << "[dispatch] hourly budget exceeded ($"
                          << cost_h << " >= $" << cfg.budget.hourly_limit_usd
                          << "), pausing dispatch\n";
            }
            return;
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

        // Look up agent metadata by id
        auto task_agent_name = get_task_agent(db, task_id);
        sui::quorum::AgentMetadata task_agent_meta;
        for (const auto& a : cfg.agents) {
            if (a.id == task_agent_name) {
                task_agent_meta = a;
                break;
            }
        }
        auto result = invoker.invoke(task_id, task_agent_meta);

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

        // Process structured output
        if (result.success && !result.output.empty()) {
            auto agent_id = get_task_agent(db, task_id);
            if (!agent_id.empty()) {
                auto parsed = output_parser.parse(result.output);

                // Apply vault updates
                if (!parsed.vault_updates.empty()) {
                    auto applied = vault_manager.apply_all_updates(agent_id,
                        parsed.vault_updates);
                    if (verbose) {
                        std::cout << "[dispatch] task " << task_id
                                  << " — " << applied << "/" << parsed.vault_updates.size()
                                  << " vault updates applied for " << agent_id << "\n";
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
