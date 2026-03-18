#pragma once

#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include "storage/database.h"
#include "utils/config.h"
#include "utils/json.h"
#include "utils/subprocess.h"

namespace sui::quorum {

struct InvocationResult {
    bool success{false};
    std::string output;
    std::string error;
    int64_t tokens_in{0};
    int64_t tokens_out{0};
    double cost{0.0};
    std::string session_id;  // session ID used/returned for this invocation
};

// Spawns `claude -p` subprocess, reads prompt from tasks table, writes result back.
class Invoker {
public:
    explicit Invoker(Database& db) : db_(db) {}

    // Validate claude -p output. Returns error string if invalid, nullopt if valid.
    [[nodiscard]] static std::optional<std::string> validate_claude_output(
        const CommandResult& result) {
        if (result.exit_code != 0) {
            return "non-zero exit code: " + std::to_string(result.exit_code)
                + (result.output.empty() ? "" : " — " + result.output);
        }
        auto type_field = json::extract_string(result.output, "type");
        if (!type_field || *type_field != "result") {
            return "invalid JSON structure: missing or wrong \"type\" field";
        }
        return std::nullopt;  // valid
    }

    // Invoke a task by id. Reads prompt from DB, spawns claude -p, writes result back.
    [[nodiscard]] InvocationResult invoke(int64_t task_id, const AgentMetadata& agent_meta) {
        // Read prompt from tasks table
        std::string prompt;
        std::string agent;
        std::string task_session_id;
        db_.query(
            "SELECT prompt, agent, session_id FROM tasks WHERE id = ? AND status = 'active'",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, task_id);
            },
            [&](sqlite3_stmt* stmt) {
                prompt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                agent = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                auto sid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                if (sid) task_session_id = sid;
            }
        );

        if (prompt.empty()) {
            auto err = "Task " + std::to_string(task_id) + " not found or not active";
            mark_failed(task_id, err);
            return {.success = false, .error = err};
        }

        // Prepend CONTEXT.md if the agent has one and it's not already in the prompt
        if (!agent_meta.context_file.empty()) {
            std::ifstream ctx(agent_meta.context_file);
            if (ctx.is_open()) {
                std::string context{std::istreambuf_iterator<char>(ctx),
                                    std::istreambuf_iterator<char>()};
                if (!context.empty()) {
                    prompt = "# Agent Context\n\n" + context + "\n\n---\n\n" + prompt;
                }
            }
        }

        // Write prompt to temp file to avoid shell escaping issues
        auto temp_path = "/tmp/quorum_prompt_" + std::to_string(task_id) + ".txt";
        {
            std::ofstream f(temp_path, std::ios::trunc);
            if (!f.is_open()) {
                auto err = "Failed to write temp prompt file";
                mark_failed(task_id, err);
                return {.success = false, .error = err};
            }
            f << prompt;
        }

        // Determine session flag
        std::string session_flag;
        if (!task_session_id.empty()) {
            // Check if a completed task already used this session_id
            // If yes → resume (-r). If no → new session (--session-id).
            int64_t prior_uses = 0;
            db_.query(
                "SELECT COUNT(*) FROM tasks "
                "WHERE session_id = ? AND status = 'done' AND id != ?",
                [&](sqlite3_stmt* s) {
                    sqlite3_bind_text(s, 1, task_session_id.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(s, 2, task_id);
                },
                [&](sqlite3_stmt* s) {
                    prior_uses = sqlite3_column_int64(s, 0);
                }
            );
            if (prior_uses > 0) {
                session_flag = " -r " + task_session_id;
            } else {
                session_flag = " --session-id " + task_session_id;
            }
        }

        // Build command: read prompt from file, pipe to claude -p
        // env -u CLAUDECODE prevents nesting detection when daemon runs inside a Claude Code session
        // Executor agents get full tool access; all others are read-only
        std::string disallowed_tools_flag;
        if (agent_meta.agent_class != "executor") {
            disallowed_tools_flag = " --disallowedTools \"Write,Edit,NotebookEdit\"";
        }

        std::string cwd_prefix;
        if (!agent_meta.target_dir.empty()) {
            // Expand ~ to HOME
            std::string dir = agent_meta.target_dir;
            if (dir.starts_with("~/")) {
                auto home = std::getenv("HOME");
                if (home) dir = std::string(home) + dir.substr(1);
            }
            cwd_prefix = "cd " + dir + " && ";
        }

        auto cmd = cwd_prefix + "env -u CLAUDECODE cat " + temp_path
            + " | claude -p --dangerously-skip-permissions"
            + disallowed_tools_flag
            + session_flag
            + " --output-format json 2>&1";

        auto cmd_result = run_command(cmd);

        // Clean up temp file
        std::remove(temp_path.c_str());

        if (!cmd_result) {
            auto err = "claude -p process failed to launch for task " + std::to_string(task_id);
            mark_failed(task_id, err);
            return {.success = false, .error = err};
        }

        auto raw_output = std::move(cmd_result->output);
        auto exit_code = cmd_result->exit_code;

        // Layer 1: Check exit code
        if (exit_code != 0) {
            // If resuming a session failed, retry as fresh session
            if (!task_session_id.empty() && session_flag.find("-r ") != std::string::npos) {
                std::cerr << "WARNING: session resume failed for task " << task_id
                          << " (session " << task_session_id << "), retrying fresh\n";

                // Rewrite prompt to temp file (it was already cleaned up)
                {
                    std::ofstream f(temp_path, std::ios::trunc);
                    if (f.is_open()) f << prompt;
                }

                // Retry without -r, with --session-id for fresh session
                auto retry_cmd = cwd_prefix + "env -u CLAUDECODE cat " + temp_path
                    + " | claude -p --dangerously-skip-permissions"
                    + disallowed_tools_flag
                    + " --session-id " + task_session_id
                    + " --output-format json 2>&1";

                cmd_result = run_command(retry_cmd);
                std::remove(temp_path.c_str());

                if (cmd_result && cmd_result->exit_code == 0) {
                    // Retry succeeded — continue with the retry result
                    raw_output = std::move(cmd_result->output);
                    exit_code = cmd_result->exit_code;
                    // Fall through to JSON validation below
                } else {
                    auto err = "claude -p failed after retry for task " + std::to_string(task_id)
                        + (cmd_result ? ": exit " + std::to_string(cmd_result->exit_code) : "");
                    mark_failed(task_id, err);
                    return {.success = false, .error = err};
                }
            } else {
                auto err = "claude -p exited with code " + std::to_string(exit_code)
                    + " for task " + std::to_string(task_id)
                    + (raw_output.empty() ? "" : ": " + raw_output);
                mark_failed(task_id, err);
                return {.success = false, .error = err};
            }
        }

        // Layer 2: Validate JSON structure — claude -p --output-format json
        // always returns {"type":"result",...} on success
        auto type_field = json::extract_string(raw_output, "type");
        if (!type_field || *type_field != "result") {
            auto err = "claude -p returned invalid JSON for task " + std::to_string(task_id)
                + ": " + raw_output.substr(0, 200);
            mark_failed(task_id, err);
            return {.success = false, .error = err};
        }

        // Parse token usage from validated JSON output
        int64_t tokens_in = json::extract_int(raw_output, "input_tokens");
        int64_t tokens_out = json::extract_int(raw_output, "output_tokens");
        double cost = json::extract_number(raw_output, "total_cost_usd");

        // Extract the result text from JSON
        auto result_text = json::extract_string(raw_output, "result");
        std::string output_text = result_text.value_or(raw_output);

        // Extract session_id from claude -p JSON output
        auto returned_session_id = json::extract_string(raw_output, "session_id");
        std::string effective_session_id = returned_session_id.value_or(task_session_id);

        // Update task in DB
        mark_done(task_id, output_text, tokens_in, tokens_out, cost);

        return {
            .success = true,
            .output = output_text,
            .error = {},
            .tokens_in = tokens_in,
            .tokens_out = tokens_out,
            .cost = cost,
            .session_id = effective_session_id,
        };
    }

private:
    Database& db_;

    void mark_done(int64_t task_id, const std::string& result,
                   int64_t tokens_in, int64_t tokens_out, double cost) {
        db_.execute(
            "UPDATE tasks SET status = 'done', result = ?, "
            "token_in = ?, token_out = ?, cost = ?, "
            "completed_at = datetime('now') "
            "WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, result.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 2, tokens_in);
                sqlite3_bind_int64(stmt, 3, tokens_out);
                sqlite3_bind_double(stmt, 4, cost);
                sqlite3_bind_int64(stmt, 5, task_id);
            }
        );
    }

    void mark_failed(int64_t task_id, const std::string& error) {
        db_.execute(
            "UPDATE tasks SET status = 'failed', error = ?, "
            "completed_at = datetime('now') WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, error.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 2, task_id);
            }
        );
    }

};

} // namespace sui::quorum
