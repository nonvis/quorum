#pragma once

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include "storage/database.h"
#include "utils/json.h"

namespace sui::quorum {

struct InvocationResult {
    bool success{false};
    std::string output;
    std::string error;
    int64_t tokens_in{0};
    int64_t tokens_out{0};
    double cost{0.0};
};

struct CommandResult {
    std::string output;
    int exit_code{-1};
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
    [[nodiscard]] InvocationResult invoke(int64_t task_id) {
        // Read prompt from tasks table
        std::string prompt;
        std::string agent;
        db_.query(
            "SELECT prompt, agent FROM tasks WHERE id = ? AND status = 'active'",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, task_id);
            },
            [&](sqlite3_stmt* stmt) {
                prompt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                agent = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            }
        );

        if (prompt.empty()) {
            auto err = "Task " + std::to_string(task_id) + " not found or not active";
            mark_failed(task_id, err);
            return {.success = false, .error = err};
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

        // Build command: read prompt from file, pipe to claude -p
        auto cmd = "cat " + temp_path
            + " | claude -p --dangerously-skip-permissions --output-format json 2>&1";

        auto cmd_result = run_command(cmd);

        // Clean up temp file
        std::remove(temp_path.c_str());

        if (!cmd_result) {
            auto err = "claude -p process failed to launch for task " + std::to_string(task_id);
            mark_failed(task_id, err);
            return {.success = false, .error = err};
        }

        auto& [raw_output, exit_code] = *cmd_result;

        // Layer 1: Check exit code
        if (exit_code != 0) {
            auto err = "claude -p exited with code " + std::to_string(exit_code)
                + " for task " + std::to_string(task_id)
                + (raw_output.empty() ? "" : ": " + raw_output);
            mark_failed(task_id, err);
            return {.success = false, .error = err};
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

        // Update task in DB
        mark_done(task_id, output_text, tokens_in, tokens_out, cost);

        return {
            .success = true,
            .output = output_text,
            .error = {},
            .tokens_in = tokens_in,
            .tokens_out = tokens_out,
            .cost = cost,
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

    // Run a shell command and capture stdout + exit code
    [[nodiscard]] std::optional<CommandResult> run_command(const std::string& cmd) {
        std::array<char, 4096> buffer;
        std::string output;

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return std::nullopt;

        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            output += buffer.data();
        }

        int status = pclose(pipe);
        int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return CommandResult{.output = std::move(output), .exit_code = exit_code};
    }
};

} // namespace sui::quorum
