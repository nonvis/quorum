#pragma once

// Phase 8 Track 5 — `quorum benchmark` subcommand.
//
// Drives the synthetic benchmark suite for a role-specialty against the
// standard team pipeline (leader -> <role-specialty> doer -> evaluator ->
// scribe), then surfaces the evaluator's score from the evaluations table.
//
// Usage:
//   quorum benchmark --role move-dev --task move-counter
//   quorum benchmark --role move-dev                     // all benchmarks
//
// Per-task flow:
//   1. Locate templates/benchmarks/<role>/<task>/. Bail with clear error
//      if missing.
//   2. Read task.md, extract frontmatter (name, description) and the body.
//      The body becomes the conversation goal seeded into `converse`.
//   3. mkdir tempdir (unique per-pid + per-task), copy expected/ into it
//      if present (starter files for the agent to modify).
//   4. Programmatically run `init_project()` inside the tempdir.
//   5. Scaffold three additional agents via `create_agent(no_ai=true)`:
//      a doer with id == role-specialty, an evaluator, and a scribe.
//   6. Write .quorum/teams/benchmark.yaml with the four-agent path.
//   7. Resolve the rubric via `resolve_rubric()`. Bail if absent.
//   8. Spawn `<self> converse --team benchmark <goal>`. The daemon loop
//      runs to completion (state == done) and exits.
//   9. Open the tempdir's quorum.db, query the latest evaluations row,
//      extract score_total. Default to NaN if no row landed (treated as
//      a failure score).
//   10. rm -rf the tempdir.
//
// --dry-run skips step 8 (and therefore step 9 / score retrieval). It's
// the smoke-test path: validate the temp project, the rubric resolves,
// the team config writes — without spending claude -p tokens.
//
// Tests don't exercise the spawn path; they exercise the rubric override
// resolution + the parser. Real benchmark runs (Track 9) hit the daemon.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <sqlite3.h>
#include <unistd.h>

#include "agent/rubric.h"
#include "cli/agent_create.h"
#include "cli/init.h"
#include "utils/subprocess.h"

namespace fs = std::filesystem;
namespace sui::quorum::cli {

namespace benchmark_detail {

// Locate templates/benchmarks/<role>/. Mirrors the resolver ladder used by
// resolve_rubric() so unit tests + builds run from build/ still find the
// shipped templates.
[[nodiscard]] inline std::optional<fs::path>
locate_benchmarks_dir(const std::string& role_specialty) {
    const std::vector<fs::path> candidates = {
        fs::path("templates") / "benchmarks" / role_specialty,
        fs::path("..") / "templates" / "benchmarks" / role_specialty,
        fs::path("..") / ".." / "templates" / "benchmarks" / role_specialty,
    };
    for (const auto& c : candidates) {
        if (fs::exists(c) && fs::is_directory(c)) {
            return c;
        }
    }
    return std::nullopt;
}

// Strip leading/trailing whitespace.
[[nodiscard]] inline std::string trim(std::string_view sv) {
    size_t b = 0;
    size_t e = sv.size();
    while (b < e && (sv[b] == ' ' || sv[b] == '\t' || sv[b] == '\r' ||
                     sv[b] == '\n')) ++b;
    while (e > b && (sv[e - 1] == ' ' || sv[e - 1] == '\t' ||
                     sv[e - 1] == '\r' || sv[e - 1] == '\n')) --e;
    return std::string(sv.substr(b, e - b));
}

// Read entire task.md, return body with the YAML frontmatter stripped.
// Returns empty string if the file is missing or has no readable content.
// The frontmatter (`---\n...\n---\n`) is dropped; everything after is the
// goal payload the daemon receives.
[[nodiscard]] inline std::string read_task_body(const fs::path& task_md) {
    std::ifstream f(task_md);
    if (!f.is_open()) return {};
    std::ostringstream oss;
    oss << f.rdbuf();
    auto content = oss.str();

    // Strip frontmatter if present.
    if (content.rfind("---", 0) == 0) {
        // Find closing '---' on its own line.
        auto end = content.find("\n---", 3);
        if (end != std::string::npos) {
            // Skip past the closing '---' and any trailing newline.
            auto after = end + 4;  // length of "\n---"
            if (after < content.size() && content[after] == '\n') {
                ++after;
            }
            return content.substr(after);
        }
    }
    return content;
}

// Recursively copy `src` directory into `dst` (preserving structure). No-op
// if `src` doesn't exist. Used to seed expected/ starter files into the
// temp project.
inline void copy_dir_recursive(const fs::path& src, const fs::path& dst) {
    if (!fs::exists(src) || !fs::is_directory(src)) return;
    fs::create_directories(dst);
    for (const auto& entry : fs::recursive_directory_iterator(src)) {
        const auto& p = entry.path();
        auto rel = fs::relative(p, src);
        auto target = dst / rel;
        if (entry.is_directory()) {
            fs::create_directories(target);
        } else if (entry.is_regular_file()) {
            fs::create_directories(target.parent_path());
            fs::copy_file(p, target, fs::copy_options::overwrite_existing);
        }
    }
}

// List all benchmark subdirectories under templates/benchmarks/<role>/.
// Returns the sorted list of task names (subdir names containing task.md).
[[nodiscard]] inline std::vector<std::string>
list_tasks(const fs::path& benchmarks_dir) {
    std::vector<std::string> tasks;
    if (!fs::exists(benchmarks_dir)) return tasks;
    for (const auto& entry : fs::directory_iterator(benchmarks_dir)) {
        if (!entry.is_directory()) continue;
        auto task_md = entry.path() / "task.md";
        if (fs::exists(task_md)) {
            tasks.push_back(entry.path().filename().string());
        }
    }
    std::sort(tasks.begin(), tasks.end());
    return tasks;
}

// Query the most recent evaluations row from the project's quorum.db and
// return the score_total. Returns nullopt if no evaluation row landed.
[[nodiscard]] inline std::optional<double>
read_latest_score(const fs::path& project_root) {
    auto db_path = project_root / ".quorum" / "quorum.db";
    if (!fs::exists(db_path)) return std::nullopt;

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(db_path.string().c_str(), &db,
                        SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return std::nullopt;
    }

    std::optional<double> score;
    sqlite3_stmt* stmt = nullptr;
    auto sql = "SELECT score_total FROM evaluations "
               "ORDER BY id DESC LIMIT 1";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            score = sqlite3_column_double(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return score;
}

// Format a score for the per-task tabular output. "—" marker when no
// evaluation row landed (failure / dry-run).
[[nodiscard]] inline std::string format_score(std::optional<double> score) {
    if (!score) return "—";
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(0) << *score;
    return oss.str();
}

}  // namespace benchmark_detail

// Set up a temp project for one benchmark task and (unless dry_run) drive
// the conversation through the daemon. Returns the score_total from the
// evaluations table, or nullopt if the run didn't produce a score.
[[nodiscard]] inline std::optional<double>
run_one_benchmark(const std::string& role_specialty,
                  const std::string& task_name,
                  const fs::path& benchmarks_dir,
                  bool dry_run,
                  bool verbose,
                  bool keep_tempdir = false) {
    namespace bd = benchmark_detail;

    // 1. Locate task dir + read task.md
    auto task_dir = benchmarks_dir / task_name;
    auto task_md = task_dir / "task.md";
    if (!fs::exists(task_md)) {
        std::cerr << "ERROR: task.md not found at " << task_md << "\n";
        return std::nullopt;
    }

    auto goal = bd::read_task_body(task_md);
    if (bd::trim(goal).empty()) {
        std::cerr << "ERROR: task.md body empty after frontmatter strip: "
                  << task_md << "\n";
        return std::nullopt;
    }

    // 2. Resolve rubric availability. We resolve from the SHIPPED templates
    //    side first since the temp project's .quorum/rubrics/ is empty by
    //    construction. Note: the resolver also checks project_root override,
    //    so a project rubric at .quorum/rubrics/<role>/rubric.md would win
    //    if we ever copy one in. For now, the template resolution suffices.
    auto rubric = sui::quorum::resolve_rubric(/*project_root=*/"",
                                              role_specialty);
    if (!rubric) {
        std::cerr << "ERROR: no rubric found for role-specialty '"
                  << role_specialty << "'. Expected at "
                  << "templates/rubrics/" << role_specialty << "/rubric.md "
                  << "or .quorum/rubrics/" << role_specialty
                  << "/rubric.md.\n";
        return std::nullopt;
    }
    if (verbose) {
        std::cout << "  Rubric: " << rubric->name << " " << rubric->version
                  << " (" << rubric->items.size() << " items, "
                  << rubric->total_weight << " total weight)\n";
    }

    // Resolve the rubric's source path to absolute *before* we chdir into
    // the tempdir, so the later copy step still finds it.
    fs::path rubric_source_abs;
    try {
        rubric_source_abs = fs::absolute(rubric->source_path);
    } catch (const fs::filesystem_error& e) {
        std::cerr << "ERROR: failed to resolve rubric source path '"
                  << rubric->source_path << "': " << e.what() << "\n";
        return std::nullopt;
    }

    // 3. Create temp dir
    char tmpl[] = "/tmp/quorum-bench-XXXXXX";
    if (mkdtemp(tmpl) == nullptr) {
        std::cerr << "ERROR: mkdtemp failed: " << std::strerror(errno) << "\n";
        return std::nullopt;
    }
    fs::path tempdir(tmpl);
    if (verbose) {
        std::cout << "  Tempdir: " << tempdir.string() << "\n";
    }

    // 4. Copy expected/ starter files (if present)
    bd::copy_dir_recursive(task_dir / "expected", tempdir);

    // 5. chdir into tempdir to run init/agent-create (those helpers operate
    //    on CWD). Save and restore the original CWD on exit.
    auto saved_cwd = fs::current_path();
    auto cleanup = [&]() {
        try {
            fs::current_path(saved_cwd);
            if (keep_tempdir) {
                std::cout << "  Tempdir kept (--keep-tempdir): "
                          << tempdir.string() << "\n";
            } else {
                fs::remove_all(tempdir);
            }
        } catch (...) {
            // best-effort cleanup
        }
    };

    fs::current_path(tempdir);

    // 6. quorum init
    {
        auto rc = sui::quorum::cli::init_project();
        if (rc != 0) {
            std::cerr << "ERROR: quorum init failed in tempdir\n";
            cleanup();
            return std::nullopt;
        }
    }

    // 7. Scaffold doer + evaluator + scribe agents (no-AI mode for speed)
    auto scaffold = [&](const std::string& role,
                        const std::string& name) -> bool {
        sui::quorum::cli::AgentCreateParams p;
        p.role = role;
        p.name = name;
        p.no_ai = true;
        if (role == "doer") {
            p.target_dir = ".";
        }
        auto rc = sui::quorum::cli::create_agent(p);
        return rc == 0;
    };

    if (!scaffold("doer", role_specialty)) {
        std::cerr << "ERROR: failed to scaffold doer agent '"
                  << role_specialty << "'\n";
        cleanup();
        return std::nullopt;
    }
    if (!scaffold("evaluator", "evaluator")) {
        std::cerr << "ERROR: failed to scaffold evaluator agent\n";
        cleanup();
        return std::nullopt;
    }

    // Place the rubric where the evaluator's context assembler will preload
    // it. Without this, the evaluator agent has no in-context handle to its
    // rubric and previously fell into a filesystem-scan loop. The auto-
    // preloaded `rule-*-rubric.md` knowledge file ships in every turn.
    {
        fs::path rubric_dest = tempdir
            / ".quorum" / "vaults" / "evaluator" / "knowledge"
            / ("rule-" + role_specialty + "-rubric.md");
        try {
            fs::create_directories(rubric_dest.parent_path());
            fs::copy_file(rubric_source_abs, rubric_dest,
                          fs::copy_options::overwrite_existing);
            if (verbose) {
                std::cout << "  Copied rubric to: "
                          << rubric_dest.string() << "\n";
            }
        } catch (const fs::filesystem_error& e) {
            std::cerr << "ERROR: failed to copy rubric to evaluator vault: "
                      << e.what() << "\n";
            cleanup();
            return std::nullopt;
        }
    }

    if (!scaffold("scribe", "scribe")) {
        std::cerr << "ERROR: failed to scaffold scribe agent\n";
        cleanup();
        return std::nullopt;
    }

    // 8. Write benchmark team yaml
    {
        std::ofstream out(tempdir / ".quorum" / "teams" / "benchmark.yaml",
                          std::ios::trunc);
        out << "name: Benchmark\n"
            << "default_path: [leader, " << role_specialty
            << ", evaluator, scribe]\n";
    }

    if (dry_run) {
        std::cout << "  Dry run: tempdir prepared at " << tempdir.string()
                  << "\n  Skipping daemon spawn — no score retrieved.\n";
        cleanup();
        return std::nullopt;
    }

    // 9. Drive the conversation. We spawn the daemon binary located at
    //    argv[0] of the parent process (set via QUORUM_DAEMON_PATH env var
    //    if present, else fall back to "quorum_daemon" on PATH).
    std::string daemon_path = "quorum_daemon";
    if (auto env = std::getenv("QUORUM_DAEMON_PATH")) {
        daemon_path = env;
    }

    // Single-quote the goal for shell safety. Replace internal single quotes
    // with the standard '"'"' escape pattern.
    std::string escaped_goal;
    escaped_goal.reserve(goal.size() + 8);
    for (char c : goal) {
        if (c == '\'') escaped_goal += "'\"'\"'";
        else escaped_goal += c;
    }

    auto cmd = daemon_path
        + " converse --once --team benchmark '" + escaped_goal + "'";
    if (verbose) {
        std::cout << "  Spawning: " << cmd << "\n";
    }
    auto result = sui::quorum::run_command(cmd);
    if (!result || result->exit_code != 0) {
        std::cerr << "WARNING: daemon spawn failed or exited non-zero "
                  << "(exit_code="
                  << (result ? result->exit_code : -1)
                  << "). No score will be retrieved.\n";
        // Fall through — we still try to read evaluations in case partial
        // progress landed a row.
    }

    // 10. Read score
    auto score = bd::read_latest_score(tempdir);

    cleanup();
    return score;
}

// Aggregate output: per-task table + mean + median.
inline void print_aggregate(const std::string& role_specialty,
                             const std::vector<std::pair<std::string,
                                 std::optional<double>>>& results) {
    std::cout << "Benchmark suite: " << role_specialty << " ("
              << results.size() << " tasks)\n";
    std::cout << "─────────────────────────────────────────────\n";

    // Find the longest task name for column alignment.
    size_t name_width = 0;
    for (const auto& [n, _] : results) {
        name_width = std::max(name_width, n.size());
    }
    name_width = std::max<size_t>(name_width, 12);

    std::vector<double> scored;
    for (const auto& [name, score] : results) {
        std::cout << "  " << std::left << std::setw(static_cast<int>(name_width + 2))
                  << name << benchmark_detail::format_score(score) << "\n";
        if (score) scored.push_back(*score);
    }

    std::cout << "─────────────────────────────────────────────\n";

    if (scored.empty()) {
        std::cout << "Mean: —   Median: —   (no scores recorded)\n";
        return;
    }

    double sum = 0;
    for (double s : scored) sum += s;
    double mean = sum / scored.size();

    auto sorted = scored;
    std::sort(sorted.begin(), sorted.end());
    double median;
    if (sorted.size() % 2 == 1) {
        median = sorted[sorted.size() / 2];
    } else {
        median = (sorted[sorted.size() / 2 - 1] +
                  sorted[sorted.size() / 2]) / 2.0;
    }

    std::cout << "Mean: " << std::fixed << std::setprecision(1) << mean
              << "\n"
              << "Median: " << std::fixed << std::setprecision(0) << median
              << "\n";
}

// Top-level entry point. Returns 0 on success, non-zero on configuration
// error. Note: a benchmark that runs but scores poorly is still a success
// (return 0). Failure means the benchmark didn't run at all (missing
// task, missing rubric, mkdtemp failure, etc.).
inline int run_benchmark(const std::string& role_specialty,
                         const std::string& task_name = "",
                         bool dry_run = false,
                         bool verbose = false,
                         bool keep_tempdir = false) {
    namespace bd = benchmark_detail;

    if (role_specialty.empty()) {
        std::cerr << "ERROR: --role <role-specialty> is required\n";
        return 1;
    }

    auto benchmarks_dir_opt = bd::locate_benchmarks_dir(role_specialty);
    if (!benchmarks_dir_opt) {
        std::cerr << "ERROR: no benchmarks directory found for role '"
                  << role_specialty << "'. Expected at "
                  << "templates/benchmarks/" << role_specialty << "/.\n";
        return 1;
    }
    auto& benchmarks_dir = *benchmarks_dir_opt;

    // Single task path
    if (!task_name.empty()) {
        auto task_dir = benchmarks_dir / task_name;
        if (!fs::exists(task_dir / "task.md")) {
            std::cerr << "ERROR: task '" << task_name << "' not found "
                      << "(no task.md at " << task_dir.string() << "/)\n";
            // List available tasks to help the operator.
            auto tasks = bd::list_tasks(benchmarks_dir);
            if (!tasks.empty()) {
                std::cerr << "Available tasks:\n";
                for (const auto& t : tasks) {
                    std::cerr << "  - " << t << "\n";
                }
            }
            return 1;
        }

        std::cout << "Running benchmark: " << role_specialty
                  << "/" << task_name << "\n";
        auto score = run_one_benchmark(role_specialty, task_name,
                                        benchmarks_dir, dry_run, verbose,
                                        keep_tempdir);
        if (score) {
            std::cout << "Score: " << std::fixed << std::setprecision(0)
                      << *score << "\n";
        } else if (!dry_run) {
            std::cout << "Score: — (no evaluation row recorded)\n";
        }
        return 0;
    }

    // Aggregate run: enumerate tasks, run each
    auto tasks = bd::list_tasks(benchmarks_dir);
    if (tasks.empty()) {
        std::cerr << "ERROR: no benchmark tasks found under "
                  << benchmarks_dir.string() << "/\n";
        return 1;
    }

    std::vector<std::pair<std::string, std::optional<double>>> results;
    results.reserve(tasks.size());
    for (const auto& t : tasks) {
        std::cout << "[" << (results.size() + 1) << "/" << tasks.size()
                  << "] " << t << "\n";
        auto score = run_one_benchmark(role_specialty, t,
                                        benchmarks_dir, dry_run, verbose,
                                        keep_tempdir);
        results.push_back({t, score});
    }

    std::cout << "\n";
    print_aggregate(role_specialty, results);
    return 0;
}

}  // namespace sui::quorum::cli
