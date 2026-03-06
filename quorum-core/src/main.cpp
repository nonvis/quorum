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
#include "daemon/router.h"
#include "daemon/event_dispatcher.h"
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

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " --config <path>\n"
              << "  --config <path>   Path to quorum.yaml config file\n"
              << "  --verbose         Enable verbose logging\n"
              << "  --help            Show this message\n";
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
        "CREATE TABLE IF NOT EXISTS proposals ("
        "  id TEXT PRIMARY KEY,"
        "  title TEXT NOT NULL,"
        "  author TEXT NOT NULL,"
        "  state INTEGER NOT NULL DEFAULT 0,"
        "  created_at INTEGER NOT NULL,"
        "  updated_at INTEGER NOT NULL,"
        "  current_round INTEGER NOT NULL DEFAULT 0,"
        "  walrus_blob_id TEXT"
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
        "  completed_at TEXT"
        ")"
    );
    db.execute(
        "CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status)"
    );
    db.execute(
        "CREATE INDEX IF NOT EXISTS idx_tasks_agent ON tasks(agent)"
    );
}

// Insert a new task into the queue. Returns the task id.
static int64_t enqueue_task(sui::quorum::Database& db,
                            const std::string& agent,
                            const std::string& task_type,
                            const std::string& prompt) {
    db.execute(
        "INSERT INTO tasks (agent, task_type, status, prompt) VALUES (?, ?, 'pending', ?)",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_text(stmt, 1, agent.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, task_type.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, prompt.c_str(), -1, SQLITE_TRANSIENT);
        }
    );
    return db.last_insert_id();
}

// Count currently active (running) tasks
static int64_t count_active_tasks(sui::quorum::Database& db) {
    return db.query_int("SELECT COUNT(*) FROM tasks WHERE status = 'active'");
}

// Get daily cost so far
static double daily_cost(sui::quorum::Database& db) {
    return db.query_double(
        "SELECT COALESCE(SUM(cost), 0.0) FROM tasks "
        "WHERE created_at > datetime('now', '-1 day')"
    );
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

int main(int argc, char* argv[]) {
    std::cout << "Quorum Daemon v0.2.0" << std::endl;
    std::cout << "====================" << std::endl;

    // Parse CLI args
    std::string config_path;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
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

    std::cout << "  Network:    " << cfg.chain.network << "\n";
    std::cout << "  Data dir:   " << cfg.daemon.data_dir << "\n";
    std::cout << "  Log level:  " << cfg.daemon.log_level << "\n";
    std::cout << "  Agents:     " << cfg.agents.size() << "\n";
    std::cout << "  Max parallel: " << cfg.budget.max_concurrent << "\n";
    std::cout << "  Daily budget: $" << cfg.budget.daily_limit_usd << "\n";

    // Signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // PID lock
    if (!acquire_pid_lock(cfg.daemon.pid_file)) return 1;

    // Ensure data directory exists
    fs::create_directories(cfg.daemon.data_dir);

    // Initialize database
    auto db_path = cfg.daemon.data_dir + "/quorum.db";
    sui::quorum::Database db(db_path);
    if (!db.is_open()) {
        std::cerr << "ERROR: Failed to open database: " << db_path << "\n";
        release_pid_lock(cfg.daemon.pid_file);
        return 1;
    }
    init_schema(db);

    if (verbose) {
        std::cout << "  Database:   " << db_path << " (OK)\n";
    }

    // Initialize subsystems
    sui::quorum::Scheduler scheduler;
    sui::quorum::MessageBus message_bus;
    sui::quorum::Router router;
    sui::quorum::EventDispatcher events;
    sui::quorum::Invoker invoker(db);
    sui::quorum::ContextAssembler context_assembler;
    sui::quorum::OutputParser output_parser;
    sui::quorum::VaultManager vault_manager(cfg.daemon.data_dir);

    // Initialize vaults for all configured agents
    for (const auto& agent_ref : cfg.agents) {
        auto agent_id = fs::path(agent_ref.config_path).stem().string();
        bool ok = vault_manager.init_vault(agent_id);
        if (verbose) {
            std::cout << "  Vault init: " << agent_id
                      << (ok ? " (OK)" : " (FAILED)") << "\n";
        }
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
        auto cost_d = daily_cost(db);

        if (verbose) {
            std::cout << "[" << format_ts(epoch_seconds()) << "] heartbeat"
                      << " — active: " << active
                      << " pending_msgs: " << message_bus.pending()
                      << " cost_1h: $" << cost_h
                      << " cost_24h: $" << cost_d
                      << "\n";
        }
    });

    // Task dispatch: check for pending tasks and invoke them
    scheduler.add("task_dispatch", 5, [&]() {
        // Check budget
        auto cost_d = daily_cost(db);
        if (cost_d >= cfg.budget.daily_limit_usd) {
            if (verbose) {
                std::cout << "[dispatch] daily budget exceeded ($"
                          << cost_d << " >= $" << cfg.budget.daily_limit_usd
                          << "), pausing dispatch\n";
            }
            return;
        }

        // Check parallelism
        auto active = count_active_tasks(db);
        if (active >= static_cast<int64_t>(cfg.budget.max_concurrent)) {
            return;  // at capacity
        }

        // Claim and dispatch
        auto task_id = claim_next_task(db);
        if (task_id == 0) return;  // no pending tasks

        if (verbose) {
            std::cout << "[dispatch] invoking task " << task_id << "\n";
        }

        auto result = invoker.invoke(task_id);

        if (verbose) {
            if (result.success) {
                std::cout << "[dispatch] task " << task_id << " done"
                          << " — tokens_in: " << result.tokens_in
                          << " tokens_out: " << result.tokens_out
                          << " cost: $" << result.cost << "\n";
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

                // TODO: handle parsed.proposals (Action Item #5)
                // TODO: handle parsed.reviews (Action Item #6)
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
    auto total_cost = daily_cost(db);
    std::cout << "  Tasks completed: " << total_done << "\n";
    std::cout << "  Tasks failed:    " << total_failed << "\n";
    std::cout << "  Cost (24h):      $" << total_cost << "\n";

    release_pid_lock(cfg.daemon.pid_file);
    std::cout << "Shutdown complete." << std::endl;
    return 0;
}
