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
#include "cli/init.h"
#include "cli/skills.h"
#include "utils/discover.h"

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
              << "  " << prog << " converse \"goal text\"                      Start conversation (auto-discovers .quorum/)\n"
              << "  " << prog << " converse --team quick-build \"fix bug\"     Use specific team\n"
              << "  " << prog << " teams                                      List available teams\n"
              << "  " << prog << " skills                                     List available Claude Code skills\n"
              << "  " << prog << " status                                    List conversations\n"
              << "  " << prog << " --config <path>                            Start daemon\n"
              << "  " << prog << " --config <path> converse --budget 3.0 \"g\"  Custom budget\n"
              << "  " << prog << " --config <path> resume --conversation <id> Resume paused\n"
              << "  " << prog << " --config <path> close --conversation <id>  Close conversation\n"
              << "  " << prog << " --config <path> respond --conversation <id> \"text\"  Respond to human request\n"
              << "  " << prog << " --config <path> agent create --role <r> --name <n> --project <p>\n"
              << "  " << prog << " agent modify --name <id> --description \"new desc\"   Modify agent\n"
              << "  " << prog << " agent list                                            List all agents\n"
              << "\nOptions:\n"
              << "  --config <path>      Path to config YAML (optional if .quorum/ exists in project)\n"
              << "  --verbose            Enable verbose logging\n"
              << "  --budget <usd>       Per-conversation budget (default: 5.0)\n"
              << "  --max-rounds <n>     Max revision rounds (default: 3)\n"
              << "  --team <name>        Team preset from .quorum/teams/ (optional)\n"
              << "  --conversation <id>  Conversation ID for resume/close\n"
              << "  --help               Show this message\n"
              << "\nAgent create options:\n"
              << "  --role <role>        Agent role (leader|thinker|doer|reviewer|scribe|librarian)\n"
              << "  --name <name>        Agent ID\n"
              << "  --project <name>     Project subfolder in configs/agents/ (optional with .quorum/)\n"
              << "  --description <d>    Agent description (optional)\n"
              << "  --skill-file <path>  Path to SKILL.md (optional)\n"
              << "  --skill <name>       Shorthand for --skill-file .claude/skills/<name>/SKILL.md\n"
              << "  --target-dir <path>  Working directory for doer agents (optional)\n"
              << "  --no-ai              Skip AI generation, copy template as-is\n"
              << "  --regenerate         Regenerate CONTEXT.md without changing fields\n";
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
    double conv_budget = -1.0;        // sentinel — will use config default
    int conv_max_rounds = -1;         // sentinel — will use config default
    int64_t conv_id_arg = 0;
    std::string goal_text;
    std::string response_text;
    std::string agent_subcmd;
    sui::quorum::cli::AgentCreateParams agent_params;
    std::string team_name;

    if (subcommand == "converse") {
        for (size_t i = 0; i < sub_args.size(); ++i) {
            if (sub_args[i] == "--budget" && i + 1 < sub_args.size()) {
                conv_budget = std::stod(sub_args[++i]);
            } else if (sub_args[i] == "--max-rounds" && i + 1 < sub_args.size()) {
                conv_max_rounds = std::stoi(sub_args[++i]);
            } else if (sub_args[i] == "--team" && i + 1 < sub_args.size()) {
                team_name = sub_args[++i];
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
            if (agent_subcmd.empty() && (sub_args[i] == "create" || sub_args[i] == "modify" || sub_args[i] == "list")) {
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
    } else if (subcommand == "teams") {
        // No additional flags needed
    } else if (subcommand == "skills") {
        // No additional flags needed
    } else if (!subcommand.empty() && subcommand != "status") {
        std::cerr << "Unknown subcommand: " << subcommand << "\n";
        print_usage(argv[0]);
        return 1;
    }

    // Init doesn't need --config -- it creates the config
    if (subcommand == "init") {
        return sui::quorum::cli::init_project();
    }

    // Teams doesn't need --config -- reads .quorum/teams/ directly
    if (subcommand == "teams") {
        auto root = sui::quorum::discover_project_root();
        if (!root) {
            std::cerr << "No .quorum/ found. Run 'quorum init' first.\n";
            return 1;
        }
        auto teams = sui::quorum::load_team_presets(
            (fs::path(*root) / ".quorum" / "teams").string());
        if (teams.empty()) {
            std::cout << "No teams configured. Create a file in .quorum/teams/:\n";
            std::cout << "  echo 'name: My Team\\ndefault_path: [leader, doer]' > .quorum/teams/my-team.yaml\n";
            return 0;
        }
        std::cout << "Teams:\n";
        for (const auto& t : teams) {
            std::cout << "  " << t.id << "  \"" << t.name << "\"    ";
            for (size_t i = 0; i < t.default_path.size(); ++i) {
                if (i > 0) std::cout << " -> ";
                std::cout << t.default_path[i];
            }
            std::cout << "\n";
        }
        return 0;
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

    // Agent list/modify don't need --config -- they work with .quorum/ directly
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

    // Load team presets from .quorum/teams/ if available
    if (fs::exists(".quorum/teams") && fs::is_directory(".quorum/teams")) {
        cfg.teams = sui::quorum::load_team_presets(".quorum/teams");
        if (verbose && !cfg.teams.empty()) {
            std::cout << "  Teams:      " << cfg.teams.size() << "\n";
            for (const auto& t : cfg.teams) {
                std::cout << "    - " << t.id << " (" << t.name << ")\n";
            }
        }
    }

    // Apply team preset if --team specified
    if (!team_name.empty()) {
        bool found = false;
        for (const auto& t : cfg.teams) {
            if (t.id == team_name) {
                cfg.conversations.default_path = t.default_path;
                found = true;
                if (verbose) {
                    std::cout << "  Using team: " << t.name << "\n";
                }
                break;
            }
        }
        if (!found) {
            std::cerr << "ERROR: team '" << team_name << "' not found. Available:\n";
            for (const auto& t : cfg.teams) {
                std::cerr << "  - " << t.id << " (" << t.name << ")\n";
            }
            if (cfg.teams.empty()) {
                std::cerr << "  (no team presets in .quorum/teams/)\n";
            }
            return 1;
        }
    }

    // ── Agent subcommand early exit (no DB, no daemon) ──────────────────
    if (subcommand == "agent") {
        if (agent_subcmd != "create") {
            std::cerr << "ERROR: unknown agent subcommand. Usage: agent create|modify|list\n";
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
                auto id = conversation_engine.start(goal_text, conv_budget, conv_max_rounds, team_name);
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

    // converse/resume dispatch (daemon path — PID lock acquired)
    if (subcommand == "converse") {
        if (goal_text.empty()) {
            std::cerr << "ERROR: converse requires a goal string\n";
            release_pid_lock(cfg.daemon.pid_file);
            return 1;
        }
        auto id = conversation_engine.start(goal_text, conv_budget, conv_max_rounds, team_name);
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
