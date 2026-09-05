#pragma once

#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include "agent/output_parser.h"
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
    // Phase 7 Track 5 — Anthropic prompt-cache token accounting.
    int64_t cache_creation_tokens{0};
    int64_t cache_read_tokens{0};
};

// Spawns `claude -p` subprocess, reads prompt from tasks table, writes result back.
class Invoker {
public:
    explicit Invoker(Database& db) : db_(db) {}

    // Validate a `claude -p --output-format json` envelope BODY (the exit code
    // is checked by the caller). Returns an error string, nullopt when healthy.
    //
    // Every read here is DEPTH-0. Claude Code 2.1.261 nests
    // usage.iterations[0]."type" == "message" ahead of the top-level
    // "type":"result", so the old flat json::extract_string(raw, "type")
    // returned "message" and rejected every healthy reply.
    [[nodiscard]] static std::optional<std::string> validate_envelope_json(
        const std::string& raw_output) {
        auto type_field = json::extract_top_level_string(raw_output, "type");
        if (!type_field || *type_field != "result") {
            return "invalid JSON structure: missing or wrong \"type\" field";
        }
        // A "type":"result" envelope can still carry a failure. Report the
        // model's own error text (which lives in "result") to the operator.
        if (json::extract_top_level_bool(raw_output, "is_error", false)) {
            auto subtype = json::extract_top_level_string(raw_output, "subtype");
            auto text = json::extract_top_level_string(raw_output, "result");
            return "claude reported is_error=true"
                + (subtype ? " (" + *subtype + ")" : std::string{})
                + (text && !text->empty() ? ": " + *text : std::string{});
        }
        return std::nullopt;  // valid
    }

    // Layer 1 of invoke(): the exit-code gate. Returns an error string on a
    // non-zero exit, nullopt when the process exited clean. The envelope BODY
    // is NOT inspected here — invoke() may retry a failed session resume before
    // any JSON is looked at, so the two gates must stay separable.
    //
    // 2026-09-04: this replaces validate_claude_output(), a wrapper that ran
    // both gates at once. That wrapper had zero callers in src/ (only tests) —
    // it was tested but never called. invoke() below calls THIS function.
    [[nodiscard]] static std::optional<std::string> validate_exit_code(
        const CommandResult& result) {
        if (result.exit_code != 0) {
            return "non-zero exit code: " + std::to_string(result.exit_code)
                + (result.output.empty() ? "" : " — " + result.output);
        }
        return std::nullopt;
    }

    // Pure parse of a claude -p envelope into an InvocationResult. No DB, no
    // subprocess — this is the exact code invoke() runs after Layer 1, so it is
    // unit-testable against real envelopes.
    //
    // Token fields are read from INSIDE the top-level "usage" object, never
    // from the whole document: usage.iterations[] repeats input_tokens /
    // output_tokens / cache_*_input_tokens per API round-trip, and "modelUsage"
    // repeats them per model. A flat scan would silently report one iteration
    // instead of the turn total.
    [[nodiscard]] static InvocationResult parse_envelope(const std::string& raw_output) {
        InvocationResult r;
        if (auto err = validate_envelope_json(raw_output)) {
            r.success = false;
            r.error = *err;
            return r;
        }
        auto usage = json::extract_top_level_object(raw_output, "usage")
                         .value_or(std::string{});
        r.tokens_in  = json::extract_top_level_int(usage, "input_tokens");
        r.tokens_out = json::extract_top_level_int(usage, "output_tokens");
        r.cache_creation_tokens =
            json::extract_top_level_int(usage, "cache_creation_input_tokens");
        r.cache_read_tokens =
            json::extract_top_level_int(usage, "cache_read_input_tokens");
        r.cost = json::extract_top_level_number(raw_output, "total_cost_usd");
        r.output = json::extract_top_level_string(raw_output, "result")
                       .value_or(raw_output);
        r.session_id = json::extract_top_level_string(raw_output, "session_id")
                           .value_or(std::string{});
        r.success = true;
        return r;
    }

    // Pure helper: build the tool-related claude -p flags based on the agent's
    // class AND the conversation mode.
    //
    // Mode override semantics (Phase 6 Track 2):
    //   - mode == "brainstorm" forces a read-only tool surface
    //     (--allowedTools "Read,Grep,Glob" + --disallowedTools "Edit,Write,Bash,NotebookEdit")
    //     for EVERY agent in the conversation, regardless of agent_class.
    //     This is intentional: a doer with agent_class: executor should NOT be
    //     able to mutate the project while the conversation is in brainstorm.
    //   - mode == "generic" (or anything unrecognized) preserves the original
    //     behavior: executor agents get full tools, all other classes get
    //     --disallowedTools "Write,Edit,NotebookEdit".
    //
    // Returns the leading-space-prefixed flag segment that gets concatenated
    // into the shell command. Extracted as a static pure function so it can
    // be unit-tested without spawning a subprocess.
    [[nodiscard]] static std::string build_tool_flags(
        const std::string& agent_class, const std::string& mode) {
        if (mode == "brainstorm") {
            // Hard read-only surface; overrides agent_class.
            return std::string(" --allowedTools \"Read,Grep,Glob\"")
                + " --disallowedTools \"Edit,Write,Bash,NotebookEdit\"";
        }
        // Generic / unrecognized mode → original behavior.
        if (agent_class != "executor") {
            return " --disallowedTools \"Write,Edit,NotebookEdit\"";
        }
        return "";  // executor in generic mode: no tool restrictions
    }

    // Phase 7 Track 5 — pure helper that builds the
    // `--append-system-prompt-file` flag segment. Returns the leading-space
    // form so it can be concatenated directly into the shell command, mirror
    // of build_tool_flags(). The path is NOT shell-escaped — the caller owns
    // a daemon-controlled temp path under /tmp (matches existing temp_path
    // handling for the prompt body), and shell escaping is intentionally not
    // applied to keep behavior identical to tool_flags() conventions.
    [[nodiscard]] static std::string build_system_prompt_flag(
        const std::string& sysprompt_path) {
        if (sysprompt_path.empty()) return "";
        return " --append-system-prompt-file " + sysprompt_path;
    }

    // Invoke a task by id. Reads prompt from DB, spawns claude -p, writes result back.
    //
    // `mode` is the conversation execution mode ("generic" or "brainstorm").
    // For non-conversation tasks (operator-seeded queue tasks) callers can leave
    // it at the default; behavior matches pre-Phase-6 invoker.
    [[nodiscard]] InvocationResult invoke(int64_t task_id, const AgentMetadata& agent_meta,
                                          const std::string& mode = "generic") {
        // Read prompt + system_prompt from tasks table.
        // Phase 7 Track 5: system_prompt is the stable identity prefix
        // (CONTEXT.md + SKILL.md + output rules), populated by the
        // ConversationEngine via assemble_split(). prompt is the per-task
        // user_message body.
        std::string prompt;
        std::string system_prompt_body;
        std::string agent;
        std::string task_session_id;
        db_.query(
            "SELECT prompt, agent, session_id, system_prompt FROM tasks "
            "WHERE id = ? AND status = 'active'",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, task_id);
            },
            [&](sqlite3_stmt* stmt) {
                prompt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                agent = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                auto sid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                if (sid) task_session_id = sid;
                auto sp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                if (sp) system_prompt_body = sp;
            }
        );

        if (prompt.empty()) {
            auto err = "Task " + std::to_string(task_id) + " not found or not active";
            mark_failed(task_id, err);
            return {.success = false, .error = err};
        }

        // Phase 7 Track 5: legacy CONTEXT.md prepend deleted. The assembler
        // (assemble_split) is now the sole source of CONTEXT.md emission and
        // routes it into system_prompt, not into the prompt body. Leaving the
        // legacy prepend in place would double-emit CONTEXT.md whenever a
        // conversation task ran (because system_prompt already contains it).

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

        // Write system_prompt to its own temp file when present. Daemon-
        // controlled path; not shell-escaped (matches temp_path convention).
        std::string sysprompt_path;
        if (!system_prompt_body.empty()) {
            sysprompt_path = "/tmp/quorum_sysprompt_" + std::to_string(task_id) + ".txt";
            std::ofstream f(sysprompt_path, std::ios::trunc);
            if (f.is_open()) {
                f << system_prompt_body;
            } else {
                // Couldn't write the sysprompt — fall back to plain invocation
                // (loses cache reuse but keeps the run going).
                sysprompt_path.clear();
            }
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
        //
        // Tool surface is determined by build_tool_flags(agent_class, mode):
        //   - generic mode: executor → full tools; others → read-only
        //   - brainstorm mode: ALL agents → read-only (overrides agent_class)
        std::string tool_flags = build_tool_flags(agent_meta.agent_class, mode);

        // Phase 7 Track 5: --append-system-prompt-file lets the stable
        // CONTEXT.md + SKILL.md + output-rules prefix benefit from
        // Anthropic's prefix-cache across consecutive turns for the same
        // agent. Empty string when sysprompt_path is empty.
        std::string sysprompt_flag = build_system_prompt_flag(sysprompt_path);

        std::string model_flag;
        if (!agent_meta.model.empty()) {
            model_flag = " --model " + agent_meta.model;
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
            + tool_flags
            + sysprompt_flag
            + model_flag
            + session_flag
            + " --output-format json 2>&1";

        auto cmd_result = run_command(cmd);

        // Clean up temp files (prompt + sysprompt)
        std::remove(temp_path.c_str());
        if (!sysprompt_path.empty()) std::remove(sysprompt_path.c_str());

        if (!cmd_result) {
            auto err = "claude -p process failed to launch for task " + std::to_string(task_id);
            mark_failed(task_id, err);
            return {.success = false, .error = err};
        }

        // Layer 1: exit-code gate. CALLED here, not re-implemented — the unit
        // tests drive validate_exit_code() directly, so the tested code is the
        // code this line runs. (Read it before the output is moved out.)
        auto exit_err = validate_exit_code(*cmd_result);

        auto raw_output = std::move(cmd_result->output);

        if (exit_err) {
            // If resuming a session failed, retry as fresh session
            if (!task_session_id.empty() && session_flag.find("-r ") != std::string::npos) {
                std::cerr << "WARNING: session resume failed for task " << task_id
                          << " (session " << task_session_id << "), retrying fresh\n";

                // Rewrite prompt + sysprompt temp files (they were cleaned up
                // on the first exit path). Track 5: same sysprompt flag so
                // the retry shares the cache prefix with the original try.
                {
                    std::ofstream f(temp_path, std::ios::trunc);
                    if (f.is_open()) f << prompt;
                }
                if (!sysprompt_path.empty()) {
                    std::ofstream f(sysprompt_path, std::ios::trunc);
                    if (f.is_open()) {
                        f << system_prompt_body;
                    } else {
                        // Match initial-write behavior on failure.
                        sysprompt_path.clear();
                        sysprompt_flag.clear();
                    }
                }

                // Retry without -r, with --session-id for fresh session
                auto retry_cmd = cwd_prefix + "env -u CLAUDECODE cat " + temp_path
                    + " | claude -p --dangerously-skip-permissions"
                    + tool_flags
                    + sysprompt_flag
                    + model_flag
                    + " --session-id " + task_session_id
                    + " --output-format json 2>&1";

                cmd_result = run_command(retry_cmd);
                std::remove(temp_path.c_str());
                if (!sysprompt_path.empty()) std::remove(sysprompt_path.c_str());

                if (cmd_result && !validate_exit_code(*cmd_result)) {
                    // Retry succeeded — continue with the retry result
                    raw_output = std::move(cmd_result->output);
                    exit_err.reset();
                    // Fall through to JSON validation below
                } else {
                    auto err = "claude -p failed after retry for task " + std::to_string(task_id)
                        + (cmd_result ? ": exit " + std::to_string(cmd_result->exit_code) : "");
                    mark_failed(task_id, err);
                    return {.success = false, .error = err};
                }
            } else {
                auto err = "claude -p failed for task " + std::to_string(task_id)
                    + ": " + *exit_err;
                mark_failed(task_id, err);
                return {.success = false, .error = err};
            }
        }

        // Layer 2: Validate + parse the envelope — claude -p --output-format
        // json always returns {"type":"result",...} on success. Both the first
        // attempt and the fresh-session retry above land here.
        //
        // parse_envelope() reads DEPTH-0 keys only. The flat by-key extractors
        // cannot be used on this envelope: Claude Code 2.1.261 puts
        // usage.iterations[0]."type" ("message") ahead of the top-level
        // "type", and repeats the token keys inside iterations[]/modelUsage.
        auto parsed = parse_envelope(raw_output);
        if (!parsed.success) {
            auto err = "claude -p returned invalid JSON for task " + std::to_string(task_id)
                + ": " + parsed.error + " — raw: " + raw_output.substr(0, 200);
            mark_failed(task_id, err);
            return {.success = false, .error = err};
        }

        // Phase 7 Track 5: Anthropic prompt-cache token accounting. All four
        // token fields come from inside the top-level "usage" object.
        int64_t tokens_in = parsed.tokens_in;
        int64_t tokens_out = parsed.tokens_out;
        int64_t cache_creation = parsed.cache_creation_tokens;
        int64_t cache_read = parsed.cache_read_tokens;
        double cost = parsed.cost;

        std::string output_text = parsed.output;

        // session_id from the envelope, falling back to the task's own.
        std::string effective_session_id =
            parsed.session_id.empty() ? task_session_id : parsed.session_id;

        // Update task in DB
        mark_done(task_id, output_text, tokens_in, tokens_out, cost,
                  cache_creation, cache_read);

        return {
            .success = true,
            .output = output_text,
            .error = {},
            .tokens_in = tokens_in,
            .tokens_out = tokens_out,
            .cost = cost,
            .session_id = effective_session_id,
            .cache_creation_tokens = cache_creation,
            .cache_read_tokens = cache_read,
        };
    }

    // The ONE completion write. `invoke()` above is its only caller in src/ —
    // of the four `UPDATE tasks SET status` sites, the other three mark a task
    // failed (here, main.cpp crash recovery) or active (main.cpp claim), so
    // this is the single place a task can carry a verdict.
    //
    // A4: `summary` is derived HERE rather than passed in, so no completion
    // path can forget it. No verdict ⇒ SQL NULL, never "" (absent ≠ empty).
    //
    // Public so a test can drive the real write without spawning `claude -p`.
    void mark_done(int64_t task_id, const std::string& result,
                   int64_t tokens_in, int64_t tokens_out, double cost,
                   int64_t cache_creation, int64_t cache_read) {
        auto summary = extract_summary(result);
        db_.execute(
            "UPDATE tasks SET status = 'done', result = ?, "
            "token_in = ?, token_out = ?, cost = ?, "
            "cache_creation_input_tokens = ?, "
            "cache_read_input_tokens = ?, "
            "summary = ?, "
            "completed_at = datetime('now') "
            "WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, result.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 2, tokens_in);
                sqlite3_bind_int64(stmt, 3, tokens_out);
                sqlite3_bind_double(stmt, 4, cost);
                sqlite3_bind_int64(stmt, 5, cache_creation);
                sqlite3_bind_int64(stmt, 6, cache_read);
                if (summary) {
                    sqlite3_bind_text(stmt, 7, summary->c_str(), -1,
                                      SQLITE_TRANSIENT);
                } else {
                    sqlite3_bind_null(stmt, 7);
                }
                sqlite3_bind_int64(stmt, 8, task_id);
            }
        );
    }

private:
    Database& db_;

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
