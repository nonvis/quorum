#pragma once

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

#include "utils/config.h"
#include "utils/subprocess.h"
#include "storage/database.h"
#include "agent/output_parser.h"
#include "agent/context_assembler.h"
#include "daemon/phase_plan_checkoff.h"

namespace sui::quorum {

// UTC date in YYYY-MM-DD form. Used as the suffix appended when the
// daemon-side checkoff backstop flips a phase-plan checkbox.
inline std::string current_date_iso() {
    auto t = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return std::string(buf);
}

enum class ConvState { active, waiting_for_human, done, closed, paused };

// Phase 14 Track 3 (Decision L1 / OQ1) — generic-mode completion recommendation.
//
// On a GENERIC conversation completing, knowledge accumulates by REFRESHING the
// affected knowers (knowers are the sole accumulators, Decision #46). Per OQ1,
// generic mode RECOMMENDS only — it never auto-spends tokens. This builds the
// one-line recommendation + the exact `quorum knower refresh` command the
// operator should run. `project` is a path/name for the --project flag (cwd-
// relative paths get a generic placeholder note). PURE: string assembly only.
inline std::string generic_refresh_recommendation(int64_t conv_id,
                                                   const std::string& project) {
    std::string p = (project.empty() || project == ".")
                        ? std::string("<project>")
                        : project;
    return "[conversation " + std::to_string(conv_id) +
           "] the codebase may have changed — refresh the knowers so they "
           "re-survey it:\n"
           "    quorum knower refresh --project " + p + " --all\n"
           "    (or a single lens: quorum knower refresh --project " + p +
           " --knower <cartographer|architect|historian|recap>)\n";
}

// Phase 14 Track 4 (Decision L4) — deterministic auto-commit backstop.
//
// The retired scribe's "Job 0" auto-committed outstanding changes on
// completion. That bookkeeping now lives in the daemon, mirroring the
// phase-plan checkoff backstop: on a conversation reaching is_done, if there
// are uncommitted changes in `target_dir`, run `git add -A && git commit`.
//
// Defensive by design: SILENT no-op on ANY git error (no repo, nothing
// staged, commit hook failure, detached HEAD, etc.) so the completion path is
// NEVER blocked. `target_dir` empty/"." falls back to `project_root`. The
// commit message embeds the conversation id + a short goal/summary label.
//
// Returns true iff a commit was actually created (for an optional one-line
// operator log); false on no-op. Generic mode only is fine — brainstorm is
// read-only and produces no working-tree changes to commit.
inline bool auto_commit_on_completion(int64_t conv_id,
                                      const std::string& target_dir,
                                      const std::string& project_root,
                                      const std::string& label) {
    std::string dir = target_dir;
    if (dir.empty() || dir == ".") dir = project_root;
    if (dir.empty()) return false;

    // Quote the dir for the shell; bail if it isn't a git work tree (silent).
    auto q = [](const std::string& s) { return "\"" + s + "\""; };
    auto in_tree = sui::quorum::run_command(
        "cd " + q(dir) +
        " && git rev-parse --is-inside-work-tree 2>/dev/null");
    if (!in_tree || in_tree->exit_code != 0) return false;

    // Anything to commit? `git status --porcelain` prints nothing on a clean
    // tree. Empty output => no-op.
    auto status = sui::quorum::run_command(
        "cd " + q(dir) + " && git status --porcelain 2>/dev/null");
    if (!status || status->output.empty()) return false;

    // Build a single-line, shell-safe commit subject. Strip any double-quotes
    // and collapse newlines so the message can't break the shell command.
    std::string subject = label;
    for (char& ch : subject) {
        if (ch == '"' || ch == '\n' || ch == '\r') ch = ' ';
    }
    if (subject.size() > 80) subject = subject.substr(0, 80);
    while (!subject.empty() && subject.back() == ' ') subject.pop_back();
    std::string msg = "Conv " + std::to_string(conv_id) +
                      (subject.empty() ? "" : ": " + subject);

    auto commit = sui::quorum::run_command(
        "cd " + q(dir) + " && git add -A && git commit -m " + q(msg) +
        " >/dev/null 2>&1");
    return commit && commit->exit_code == 0;
}

// Phase 14.1 — the brainstorm-gate invariant, as a pure predicate so it can be
// unit-tested in isolation and reused at the single VAULT_UPDATE apply site.
//
// Returns true iff a knower's VAULT_UPDATE must be SUPPRESSED because it is a
// GATED brainstorm that has NOT yet passed the human-approval gate. The daemon
// (not the LLM/SKILL) is the authority: a knower can be told to "write now"
// before approval and the daemon will still drop the write. Once a human has
// responded (gate_cleared == true) writes apply normally. Ungated scans
// (gated == false — e.g. run-knower.sh --ungated single-knower passes) and
// generic mode are NEVER suppressed by this gate. PURE: no I/O.
inline bool brainstorm_gate_suppresses_write(const std::string& mode,
                                             bool gated,
                                             bool gate_cleared) {
    return mode == "brainstorm" && gated && !gate_cleared;
}

class ConversationEngine {
public:
    ConversationEngine(Database& db, const ConversationConfig& cfg,
                       const std::vector<AgentMetadata>& agents,
                       const ContextAssembler* assembler = nullptr,
                       const std::string& project_root = {},
                       std::string agents_dir = ".quorum/agents")
        : db_(db), cfg_(cfg), agents_(agents), assembler_(assembler),
          project_root_(project_root), agents_dir_(std::move(agents_dir)) {}

    // Phase 9 finding #2 — re-scan the agents directory and replace the
    // engine's local roster with whatever's on disk now. Called at conversation
    // boundaries (start/resume/recover) so the engine picks up agents added
    // externally via `quorum agent create`. The daemon's task-dispatch loop
    // refreshes its own copy separately (main.cpp); both sides read the same
    // disk source. Safe to call when the dir doesn't exist (no-op).
    void reload_agents() {
        reload_agents_inplace(agents_, agents_dir_);
    }

    // Start a new conversation. Creates the first task for the leader agent.
    // Returns conversation ID.
    //
    // `mode` selects execution mode: "generic" (default; agents may mutate the
    // project) or "brainstorm" (project read-only; participating knowers
    // self-write their own lens's slice behind the human-approval gate).
    // Empty string falls back to cfg_.default_mode. Unknown values log a
    // warning and fall back to "generic".
    // `gated` (Phase 14.1): tri-state via int — -1 = auto (brainstorm gates
    // by default, generic never), 0 = force ungated (--ungated; single-knower
    // scans write without a human), 1 = force gated. Auto keeps single-knower
    // run-knower.sh scans (which pass --ungated) writing freely while
    // interactive `converse --mode brainstorm` gates.
    int64_t start(const std::string& goal, double budget_usd = 5.0,
                  int max_rounds = 20, const std::string& mode = "",
                  bool no_vault_write = false, int gated = -1) {
        reload_agents();  // Phase 9 finding #2 — refresh roster from disk
        auto conv_id = db_.create_conversation(goal, budget_usd, max_rounds);

        // Resolve mode: explicit arg > config default > "generic"
        std::string resolved_mode = mode.empty() ? cfg_.default_mode : mode;
        if (resolved_mode.empty()) resolved_mode = "generic";
        if (resolved_mode != "generic" && resolved_mode != "brainstorm") {
            std::cerr << "[conversation " << conv_id
                      << "] WARNING: unknown mode '" << resolved_mode
                      << "' -- falling back to 'generic'\n";
            resolved_mode = "generic";
        }
        db_.execute(
            "UPDATE conversations SET mode = ? WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, resolved_mode.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 2, conv_id);
            });

        // Phase 10 Track 5 — persist --no-vault-write so resume/recover inherit it.
        db_.execute(
            "UPDATE conversations SET no_vault_write = ? WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int(stmt, 1, no_vault_write ? 1 : 0);
                sqlite3_bind_int64(stmt, 2, conv_id);
            });

        // Phase 14.1 — resolve & persist the brainstorm gate flag. Auto (-1):
        // a brainstorm gates (interactive multi-lens human-approval default),
        // generic does not. Explicit 0/1 (--ungated / future --gated) override.
        // gate_cleared always starts 0 — only respond() flips it.
        int resolved_gated = gated;
        if (resolved_gated < 0) {
            resolved_gated = (resolved_mode == "brainstorm") ? 1 : 0;
        }
        db_.execute(
            "UPDATE conversations SET gated = ?, gate_cleared = 0 WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int(stmt, 1, resolved_gated);
                sqlite3_bind_int64(stmt, 2, conv_id);
            });

        // Determine first agent: leader > default_path[0] > agents[0]
        std::string first_agent = cfg_.leader;
        if (first_agent.empty() && !cfg_.default_path.empty()) {
            first_agent = cfg_.default_path[0];
        }
        if (first_agent.empty() && !agents_.empty()) {
            first_agent = agents_[0].id;
        }
        if (first_agent.empty()) {
            std::cerr << "[conversation " << conv_id
                      << "] ERROR: no agent available for start\n";
            return conv_id;
        }

        auto session_id = db_.get_or_create_session(conv_id, first_agent);

        std::string prompt = "# Goal\n\n" + goal + "\n\n"
            "You are the first agent in this conversation. "
            "Analyze the goal and decide the next steps.\n";

        create_task(conv_id, first_agent, "turn", prompt, session_id);
        update_current_agent(conv_id, first_agent);
        increment_turn(conv_id);

        auto display = goal.size() > 60 ? goal.substr(0, 60) : goal;
        std::cout << "[conversation " << conv_id << "] started"
                  << " -- " << display
                  << " -> " << first_agent << "\n";

        // Phase 10 Track 5 — operator-visible banner when VAULT_UPDATE writes
        // are suppressed for the lifetime of this conversation.
        if (no_vault_write) {
            std::cout << "[conversation " << conv_id
                      << "] --no-vault-write: VAULT_UPDATE writes will be SUPPRESSED for this conversation\n";
        }

        // Phase 14.1 — operator-visible banner: knower writes are held until a
        // human approves the gate (this is a gated brainstorm).
        if (resolved_gated) {
            std::cout << "[conversation " << conv_id
                      << "] gated brainstorm: knower VAULT_UPDATE writes are HELD until you approve a "
                         "waiting_for_human gate\n";
        }

        return conv_id;
    }

    // Called after a task completes. Routes to next state.
    // Returns true if conversation is still active (more tasks expected).
    bool on_task_complete(int64_t task_id, const ParsedOutput& parsed,
                          double task_cost) {
        // 1. Look up conversation for task
        auto conv_id_opt = db_.get_conversation_for_task(task_id);
        if (!conv_id_opt) return false;
        auto conv_id = *conv_id_opt;

        auto conv = db_.get_conversation(conv_id);
        if (!conv) return false;

        // Verify still active
        if (conv->state != "active") return false;

        // 2. Get agent_id from task
        std::string agent_id = get_task_agent(task_id);

        // 3. Update spent_usd
        if (task_cost > 0) {
            db_.update_conversation_spent(conv_id, task_cost);
            // Refresh conv to get updated spent
            conv = db_.get_conversation(conv_id);
            if (!conv) return false;
        }

        // Phase 14.1 — brainstorm re-entry routing. A brainstorm is hub-and-
        // spoke: the leader routes each lens, the spoke (a NON-leader knower)
        // discusses and returns the ball to the leader, and ONLY the leader
        // ends (HANDOFF to: done) or gates (HANDOFF to: human). Pre-14.1 there
        // was no path back to the leader once the team layer + default_path
        // were removed (non-leader agents are forbidden to HANDOFF to: leader),
        // so a multi-lens brainstorm collapsed to one lens then `done`. Here we
        // make the roster's own promise true ("omit HANDOFF -> the ball returns
        // to the leader"): in brainstorm mode, a non-leader's no-HANDOFF (or an
        // explicit HANDOFF to: leader) routes the ball back to the leader. The
        // leader is bounded by max_rounds/budget exactly as today.
        std::string resolved_leader = cfg_.leader;
        if (resolved_leader.empty() && !agents_.empty()) {
            resolved_leader = agents_[0].id;
        }
        bool emitter_is_leader =
            !resolved_leader.empty() && agent_id == resolved_leader;
        bool brainstorm_mode = (conv->mode == "brainstorm");
        bool handoff_targets_leader =
            parsed.handoff.has_value() &&
            !resolved_leader.empty() &&
            parsed.handoff->to == resolved_leader;
        bool brainstorm_reentry_to_leader =
            brainstorm_mode && !emitter_is_leader && !resolved_leader.empty() &&
            ( !parsed.handoff.has_value() || handoff_targets_leader );

        // 4. Determine next agent
        std::string next_agent;
        std::string next_prompt;
        bool is_done = false;
        bool is_human = false;

        if (brainstorm_reentry_to_leader) {
            // Non-leader knower returned the ball (no HANDOFF, or HANDOFF to the
            // leader). Route to the leader so it can gather the next lens, gate,
            // or end. Carry any prompt the spoke set on an explicit-to-leader
            // HANDOFF; otherwise the default continue prompt is used below.
            next_agent = resolved_leader;
            if (parsed.handoff.has_value()) next_prompt = parsed.handoff->prompt;
        } else if (parsed.handoff.has_value()) {
            // HANDOFF override
            const auto& h = *parsed.handoff;
            if (h.to == "done") {
                is_done = true;
            } else if (h.to == "human") {
                is_human = true;
                next_prompt = h.prompt;
            } else {
                next_agent = h.to;
                next_prompt = h.prompt;
                // Keep path_index in sync when HANDOFF target is in default_path
                if (!cfg_.default_path.empty()) {
                    for (int i = 0; i < static_cast<int>(cfg_.default_path.size()); ++i) {
                        if (cfg_.default_path[i] == h.to) {
                            update_path_index(conv_id, i);
                            break;
                        }
                    }
                }
            }
        } else {
            // No HANDOFF -- follow default_path or complete
            if (!cfg_.default_path.empty()) {
                int next_idx = conv->path_index + 1;
                if (next_idx < static_cast<int>(cfg_.default_path.size())) {
                    next_agent = cfg_.default_path[next_idx];
                    update_path_index(conv_id, next_idx);
                } else {
                    // Reached end of default_path
                    is_done = true;
                }
            } else {
                // No default_path, no handoff -- done
                is_done = true;
            }
        }

        // 6. Unknown agent -> leader fallback
        if (!next_agent.empty() && !is_known_agent(next_agent)) {
            std::cout << "[conversation " << conv_id
                      << "] unknown agent '" << next_agent
                      << "' -> fallback to leader\n";
            next_agent = cfg_.leader;
            if (next_agent.empty() && !agents_.empty()) {
                next_agent = agents_[0].id;
            }
        }

        // 6b. Phase 14 Track 1 (Decision L2) — brainstorm hard-rejects doers.
        // brainstorm is strictly read-only: if a resolved HANDOFF target is a
        // doer (role == "doer"), abort the conversation rather than silently
        // clamping its tool surface read-only (invoker.h defense-in-depth). The
        // operator must revise the question or switch to generic mode to build.
        if (!next_agent.empty()) {
            std::string conv_mode;
            if (auto conv_now = db_.get_conversation(conv_id)) {
                conv_mode = conv_now->mode;
            }
            if (conv_mode == "brainstorm") {
                std::string target_role;
                for (const auto& a : agents_) {
                    if (a.id == next_agent) { target_role = a.role; break; }
                }
                if (target_role == "doer") {
                    std::cout << "[conversation " << conv_id << "] "
                              << "Can't run a doer in brainstorm — revise the "
                                 "question (brainstorm is read-only; use generic "
                                 "mode to build).\n";
                    db_.complete_conversation(conv_id);
                    return false;
                }
            }
        }

        // 7. Route: done -> complete; human -> waiting_for_human; budget/turns -> pause
        if (is_done) {
            db_.complete_conversation(conv_id);
            // Deterministic phase-plan checkoff backstop — silent no-op on
            // any error so the completion path is never blocked.
            auto today = current_date_iso();
            int flipped = checkoff_completed_tasks(
                db_, conv_id, cfg_.target_dir, today);
            if (flipped > 0) {
                std::cout << "[conversation " << conv_id
                          << "] checkoff: " << flipped
                          << " plan line(s) updated\n";
            }

            // Phase 14 Track 4 (Decision L4) — deterministic auto-commit
            // backstop (absorbs the retired scribe's Job 0). Mirrors the
            // checkoff backstop's defensive style: silent no-op on any git
            // error so completion is never blocked. The goal seeds the commit
            // subject. Brainstorm is read-only so this is a no-op there.
            {
                std::string goal;
                if (auto conv_done = db_.get_conversation(conv_id)) {
                    goal = conv_done->goal;
                }
                bool committed = auto_commit_on_completion(
                    conv_id, cfg_.target_dir, project_root_, goal);
                if (committed) {
                    std::cout << "[conversation " << conv_id
                              << "] auto-committed outstanding changes\n";
                }
            }

            // Phase 14 Track 3 (OQ1) — generic-mode knowledge accumulation.
            // On generic completion the doer has (likely) shipped code, so the
            // knower vaults are now stale. Per OQ1 we RECOMMEND a refresh + print
            // the exact command; we do NOT auto-run it (no surprise token spend).
            // Brainstorm completion stays silent: the participating knowers
            // already self-wrote behind the human gate.
            {
                std::string done_mode;
                if (auto conv_done = db_.get_conversation(conv_id)) {
                    done_mode = conv_done->mode;
                }
                if (done_mode != "brainstorm") {  // generic (default) only
                    std::string proj = cfg_.target_dir;
                    if (proj.empty() || proj == ".") proj = project_root_;
                    std::cout << generic_refresh_recommendation(conv_id, proj);
                }
            }

            std::cout << "[conversation " << conv_id << "] done\n";
            return false;
        }

        if (is_human) {
            db_.update_conversation_state(conv_id, "waiting_for_human");
            update_current_agent(conv_id, "human");
            std::cout << "[conversation " << conv_id
                      << "] waiting_for_human";
            if (!next_prompt.empty()) {
                auto display = next_prompt.size() > 60
                    ? next_prompt.substr(0, 60) : next_prompt;
                std::cout << " -- " << display;
            }
            std::cout << "\n";
            return false;
        }

        // Per-conversation budget check removed — redundant with window budget + max turns.
        // The window budget (budget_window table) and max_rounds enforce cost limits.

        // Turn check
        if (conv->round >= conv->max_rounds) {
            db_.pause_conversation(conv_id, "max turns reached ("
                + std::to_string(conv->round) + "/"
                + std::to_string(conv->max_rounds) + ")");
            std::cout << "[conversation " << conv_id
                      << "] paused -- max turns reached\n";
            return false;
        }

        // 8. Create next task
        if (next_agent.empty()) {
            // Shouldn't happen but guard anyway
            db_.complete_conversation(conv_id);
            return false;
        }

        auto session_id = db_.get_or_create_session(conv_id, next_agent);

        if (next_prompt.empty()) {
            next_prompt = "Continue the conversation. Review prior turns and decide next steps.";
        }

        create_task(conv_id, next_agent, "turn", next_prompt, session_id);
        update_current_agent(conv_id, next_agent);
        increment_turn(conv_id);

        if (brainstorm_reentry_to_leader) {
            std::cout << "[conversation " << conv_id
                      << "] brainstorm: ball returned to leader (" << next_agent
                      << ") for next lens / gate\n";
        }
        std::cout << "[conversation " << conv_id
                  << "] turn " << conv->round + 1
                  << " -> " << next_agent << "\n";
        return true;
    }

    // Respond to a waiting_for_human conversation.
    // Creates a task for the leader with the human response text.
    bool respond(int64_t conversation_id, const std::string& text) {
        auto conv = db_.get_conversation(conversation_id);
        if (!conv || conv->state != "waiting_for_human") return false;

        auto leader = cfg_.leader;
        if (leader.empty() && !agents_.empty()) {
            leader = agents_[0].id;
        }
        if (leader.empty()) return false;

        // Phase 14.1 — a human responded to the waiting_for_human gate. Clear
        // the gate so the daemon stops suppressing knower VAULT_UPDATE writes
        // for this (gated brainstorm) conversation. No-op for ungated/generic.
        db_.set_gate_cleared(conversation_id, true);

        auto session_id = db_.get_or_create_session(conversation_id, leader);
        std::string prompt = "# Human Response\n\n" + text + "\n";

        create_task(conversation_id, leader, "turn", prompt, session_id);
        db_.update_conversation_state(conversation_id, "active");
        update_current_agent(conversation_id, leader);
        increment_turn(conversation_id);

        std::cout << "[conversation " << conversation_id
                  << "] human responded -> " << leader << "\n";
        return true;
    }

    // Resume a paused conversation. Dispatches to the leader.
    bool resume(int64_t conversation_id) {
        reload_agents();  // Phase 9 finding #2 — refresh roster from disk
        auto conv = db_.get_conversation(conversation_id);
        if (!conv || conv->state != "paused") return false;

        auto leader = cfg_.leader;
        if (leader.empty() && !agents_.empty()) {
            leader = agents_[0].id;
        }
        if (leader.empty()) return false;

        auto session_id = db_.get_or_create_session(conversation_id, leader);
        std::string prompt = "# Resumed\n\nThis conversation was paused: " +
            conv->goal + "\nPick up where you left off.\n";

        create_task(conversation_id, leader, "turn", prompt, session_id);
        db_.update_conversation_state(conversation_id, "active");
        update_current_agent(conversation_id, leader);
        increment_turn(conversation_id);

        db_.execute(
            "UPDATE conversations SET paused_reason = NULL WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conversation_id);
            }
        );

        std::cout << "[conversation " << conversation_id
                  << "] resumed -> " << leader << "\n";
        return true;
    }

    // Close a conversation (operator decision).
    void close(int64_t conversation_id) {
        db_.update_conversation_state(conversation_id, "closed");
        std::cout << "[conversation " << conversation_id
                  << "] closed by operator\n";
    }

    // Recover a conversation after daemon crash.
    // Re-dispatches to leader so it can decide how to proceed.
    bool recover(int64_t conversation_id) {
        reload_agents();  // Phase 9 finding #2 — refresh roster from disk
        auto conv = db_.get_conversation(conversation_id);
        if (!conv || conv->state != "active") return false;

        auto leader = cfg_.leader;
        if (leader.empty() && !agents_.empty()) {
            leader = agents_[0].id;
        }
        if (leader.empty()) return false;

        auto session_id = db_.get_or_create_session(conversation_id, leader);

        // Find the last failed agent for context
        std::string last_agent;
        db_.query(
            "SELECT agent FROM tasks WHERE conversation_id = ? AND status = 'failed' "
            "ORDER BY id DESC LIMIT 1",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conversation_id);
            },
            [&](sqlite3_stmt* stmt) {
                auto a = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (a) last_agent = a;
            }
        );

        std::string prompt = "# Recovery\n\n"
            "The daemon was restarted while this conversation was in progress. "
            "The last agent (" + (last_agent.empty() ? "unknown" : last_agent) + ") was interrupted. "
            "Review the conversation state and decide how to proceed.\n";

        create_task(conversation_id, leader, "turn", prompt, session_id);
        update_current_agent(conversation_id, leader);
        increment_turn(conversation_id);

        std::cout << "[conversation " << conversation_id
                  << "] recovered -> " << leader << "\n";
        return true;
    }

private:
    Database& db_;
    ConversationConfig cfg_;
    std::vector<AgentMetadata> agents_;
    const ContextAssembler* assembler_ = nullptr;
    std::string project_root_;
    std::string agents_dir_;  // Phase 9 finding #2 — reload source

    bool is_known_agent(const std::string& agent_id) const {
        return std::any_of(agents_.begin(), agents_.end(),
            [&](const AgentMetadata& a) { return a.id == agent_id; });
    }

    std::string get_task_agent(int64_t task_id) {
        std::string agent;
        db_.query(
            "SELECT agent FROM tasks WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, task_id);
            },
            [&](sqlite3_stmt* stmt) {
                auto a = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (a) agent = a;
            }
        );
        return agent;
    }

    void create_task(int64_t conv_id, const std::string& agent,
                     const std::string& task_type, const std::string& prompt,
                     const std::string& session_id) {
        std::string final_prompt = prompt;
        std::string final_system_prompt;

        if (assembler_) {
            // Build roster
            auto roster = ContextAssembler::build_roster(agents_, agent, cfg_);

            // Find agent metadata for vault path + skill file + role.
            // role drives Phase 7 Track 3 role-scope knowledge resolution
            // (.quorum/knowledge/roles/<role>/).
            std::string vault_dir;
            std::string skill_file;
            std::string agent_role;
            for (const auto& a : agents_) {
                if (a.id == agent) {
                    vault_dir = a.vault_path;
                    skill_file = a.skill_file;
                    agent_role = a.role;
                    break;
                }
            }

            // Phase 7 Track 5 — split system_prompt (stable identity, cached
            // by Anthropic prefix-cache) from user_message (per-task body
            // piped on stdin). The system_prompt column is read by the
            // invoker and emitted via --append-system-prompt-file.
            //
            // Phase 9 Track 1 — pass conversation_mode through to the
            // assembler so brainstorm-mode participants get the inventory
            // entries. Mode is sourced from the conversation row
            // (set by start() / start_team_with_mode()).
            std::string conv_mode;
            if (auto conv = db_.get_conversation(conv_id)) {
                conv_mode = conv->mode;
            }
            if (!vault_dir.empty()) {
                auto split = assembler_->assemble_split(
                    agent, vault_dir, task_type, prompt,
                    roster, skill_file, project_root_, agent_role,
                    /*budget=*/{}, conv_mode);
                final_system_prompt = std::move(split.system_prompt);
                final_prompt = std::move(split.user_message);
            } else {
                // No vault -- just roster + task; nothing stable to cache.
                final_prompt = roster + "\n---\n\n# Current Task\n\n" + prompt + "\n";
            }
        }

        db_.execute(
            "INSERT INTO tasks (agent, task_type, prompt, system_prompt, "
            "conversation_id, session_id) VALUES (?, ?, ?, ?, ?, ?)",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, agent.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, task_type.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, final_prompt.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, final_system_prompt.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 5, conv_id);
                sqlite3_bind_text(stmt, 6, session_id.c_str(), -1, SQLITE_TRANSIENT);
            }
        );
    }

    void update_current_agent(int64_t conv_id, const std::string& agent) {
        db_.execute(
            "UPDATE conversations SET current_agent = ? WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, agent.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 2, conv_id);
            }
        );
    }

    void increment_turn(int64_t conv_id) {
        db_.execute(
            "UPDATE conversations SET round = round + 1 WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conv_id);
            }
        );
    }

    void update_path_index(int64_t conv_id, int index) {
        db_.execute(
            "UPDATE conversations SET path_index = ? WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int(stmt, 1, index);
                sqlite3_bind_int64(stmt, 2, conv_id);
            }
        );
    }
};

} // namespace sui::quorum
