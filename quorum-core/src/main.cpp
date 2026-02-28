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

#include <unistd.h>

#include "utils/config.h"
#include "daemon/scheduler.h"
#include "daemon/message_bus.h"
#include "daemon/router.h"
#include "daemon/consensus.h"
#include "daemon/event_dispatcher.h"
#include "storage/database.h"

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

int main(int argc, char* argv[]) {
    std::cout << "Quorum Daemon v0.1.0" << std::endl;
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
    std::cout << "  RPC URL:    " << cfg.chain.rpc_url << "\n";
    std::cout << "  Data dir:   " << cfg.daemon.data_dir << "\n";
    std::cout << "  Log level:  " << cfg.daemon.log_level << "\n";
    std::cout << "  Agents:     " << cfg.agents.size() << "\n";

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

    // Create schema
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

    if (verbose) {
        std::cout << "  Database:   " << db_path << " (OK)\n";
    }

    // Initialize subsystems
    sui::quorum::Scheduler scheduler;
    sui::quorum::MessageBus message_bus;
    sui::quorum::Router router;
    sui::quorum::ConsensusEngine consensus;
    sui::quorum::EventDispatcher events;

    // Wire up event handlers
    events.on("daemon.started", [&](const std::string&) {
        if (verbose) std::cout << "[event] daemon.started\n";
    });

    events.on("scheduler.tick", [&](const std::string&) {
        message_bus.drain();
    });

    // Register scheduled tasks
    scheduler.add("heartbeat", 60, [&]() {
        if (verbose) {
            std::cout << "[" << format_ts(epoch_seconds()) << "] heartbeat — "
                      << "pending messages: " << message_bus.pending() << "\n";
        }
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
        events.emit("scheduler.tick");
    }

    std::cout << "\nShutting down..." << std::endl;
    release_pid_lock(cfg.daemon.pid_file);
    std::cout << "Shutdown complete." << std::endl;
    return 0;
}
