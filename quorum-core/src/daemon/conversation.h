#pragma once

#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "utils/uuid.h"
#include "storage/database.h"
#include "agent/output_parser.h"

namespace sui::quorum {

enum class ConvState {
    init, thinking, reviewing, approved, executing, evaluating, done, closed, paused
};

struct PauseCheck {
    bool should_pause{false};
    std::string reason;
};

class ConversationEngine {
public:
    explicit ConversationEngine(Database& db,
                                const std::string& thinker = "thinker",
                                const std::string& executor = "executor",
                                const std::string& reviewer = "reviewer")
        : db_(db), thinker_agent_(thinker), executor_agent_(executor), reviewer_agent_(reviewer) {}

    // Start a new conversation. Returns conversation ID.
    int64_t start(const std::string& goal, double budget_usd = 5.0, int max_rounds = 3,
                  const std::string& pipeline = "analyst") {
        auto conv_id = db_.create_conversation(goal, budget_usd, max_rounds, pipeline);

        auto session_id = generate_uuid();

        std::string prompt =
            "# Goal\n\n" + goal +
            "\n\n---\n\nAnalyze this goal. Produce a PROPOSAL block with your recommended approach.\n";

        db_.execute(
            "INSERT INTO tasks (agent, task_type, status, prompt, conversation_id, session_id) "
            "VALUES (?, 'think', 'pending', ?, ?, ?)",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, thinker_agent_.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, prompt.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 3, conv_id);
                sqlite3_bind_text(stmt, 4, session_id.c_str(), -1, SQLITE_TRANSIENT);
            }
        );

        db_.update_conversation_state(conv_id, "thinking");

        auto display = goal.size() > 60 ? goal.substr(0, 60) : goal;
        std::cout << "[conversation " << conv_id << "] started (" << pipeline
                  << ") — " << display << "\n";

        return conv_id;
    }

    // Called after a task completes. Routes to next state.
    // Returns true if conversation is still active, false if terminal.
    bool on_task_complete(int64_t task_id, const ParsedOutput& parsed, double task_cost) {
        auto conv_id_opt = db_.get_conversation_for_task(task_id);
        if (!conv_id_opt) return true;  // not a conversation task
        auto conv_id = *conv_id_opt;

        auto conv = db_.get_conversation(conv_id);
        if (!conv) return false;
        if (conv->state == "done" || conv->state == "closed") return false;

        // Update spent
        db_.update_conversation_spent(conv_id, task_cost);

        // Check all pause conditions BEFORE state transition
        auto pause = check_pause_conditions(conv_id, task_id, task_cost);
        if (pause.should_pause) {
            db_.pause_conversation(conv_id, pause.reason);
            std::cout << "[conversation " << conv_id << "] paused — " << pause.reason << "\n";
            return false;
        }

        // Check for agent-initiated escalation (verdict == "escalate")
        for (const auto& review : parsed.reviews) {
            std::string v = review.verdict;
            for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v == "escalate") {
                std::string reason = "agent escalation: " + review.reasoning;
                db_.pause_conversation(conv_id, reason);
                std::cout << "[conversation " << conv_id << "] paused — " << reason << "\n";
                return false;
            }
        }

        // Route based on current state
        if (conv->state == "thinking") {
            return handle_thinking(conv_id, *conv, parsed);
        } else if (conv->state == "reviewing") {
            return handle_reviewing(conv_id, *conv, parsed);
        } else if (conv->state == "approved") {
            // Phase 0.7 stub: approved → done
            db_.complete_conversation(conv_id);
            return false;
        } else if (conv->state == "executing") {
            return handle_executing(conv_id, *conv, parsed);
        } else if (conv->state == "evaluating") {
            // Phase 0.9 stub
            db_.complete_conversation(conv_id);
            return false;
        }

        return false;
    }

    // Resume a paused conversation.
    bool resume(int64_t conversation_id) {
        auto conv = db_.get_conversation(conversation_id);
        if (!conv || conv->state != "paused") return false;

        // Find the last task for this conversation
        std::string last_task_type;
        std::string last_prompt;
        std::string last_session_id;
        db_.query(
            "SELECT task_type, prompt, session_id FROM tasks "
            "WHERE conversation_id = ? ORDER BY id DESC LIMIT 1",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conversation_id);
            },
            [&](sqlite3_stmt* stmt) {
                auto tt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (tt) last_task_type = tt;
                auto p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (p) last_prompt = p;
                auto s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                if (s) last_session_id = s;
            }
        );

        if (last_task_type == "think") {
            db_.update_conversation_state(conversation_id, "thinking");
            auto session_id = last_session_id.empty() ? generate_uuid() : last_session_id;
            db_.execute(
                "INSERT INTO tasks (agent, task_type, status, prompt, conversation_id, session_id) "
                "VALUES (?, 'think', 'pending', ?, ?, ?)",
                [&](sqlite3_stmt* stmt) {
                    sqlite3_bind_text(stmt, 1, thinker_agent_.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 2, last_prompt.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(stmt, 3, conversation_id);
                    sqlite3_bind_text(stmt, 4, session_id.c_str(), -1, SQLITE_TRANSIENT);
                }
            );
        } else if (last_task_type == "review") {
            db_.update_conversation_state(conversation_id, "reviewing");
            auto session_id = generate_uuid();
            db_.execute(
                "INSERT INTO tasks (agent, task_type, status, prompt, conversation_id, session_id) "
                "VALUES (?, 'review', 'pending', ?, ?, ?)",
                [&](sqlite3_stmt* stmt) {
                    sqlite3_bind_text(stmt, 1, reviewer_agent_.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 2, last_prompt.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(stmt, 3, conversation_id);
                    sqlite3_bind_text(stmt, 4, session_id.c_str(), -1, SQLITE_TRANSIENT);
                }
            );
        } else if (last_task_type == "execute") {
            db_.update_conversation_state(conversation_id, "executing");
            auto session_id = last_session_id.empty() ? generate_uuid() : last_session_id;
            db_.execute(
                "INSERT INTO tasks (agent, task_type, status, prompt, conversation_id, session_id) "
                "VALUES (?, 'execute', 'pending', ?, ?, ?)",
                [&](sqlite3_stmt* stmt) {
                    sqlite3_bind_text(stmt, 1, executor_agent_.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 2, last_prompt.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(stmt, 3, conversation_id);
                    sqlite3_bind_text(stmt, 4, session_id.c_str(), -1, SQLITE_TRANSIENT);
                }
            );
        } else {
            return false;
        }

        // Clear paused_reason
        db_.execute(
            "UPDATE conversations SET paused_reason = NULL WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conversation_id);
            }
        );

        std::cout << "[conversation " << conversation_id << "] resumed\n";
        return true;
    }

    // Close a conversation (operator decision).
    void close(int64_t conversation_id) {
        db_.update_conversation_state(conversation_id, "closed");
        std::cout << "[conversation " << conversation_id << "] closed by operator\n";
    }

    // Approve or reject a conversation at the human gate (APPROVED state).
    // Returns true on success.
    bool gate(int64_t conversation_id, bool approve) {
        auto conv = db_.get_conversation(conversation_id);
        if (!conv || conv->state != "approved") {
            std::cerr << "ERROR: conversation " << conversation_id
                      << " is not in approved state (current: "
                      << (conv ? conv->state : "not found") << ")\n";
            return false;
        }

        if (!approve) {
            db_.update_conversation_state(conversation_id, "closed");
            std::cout << "[conversation " << conversation_id << "] rejected at human gate\n";
            return true;
        }

        // Retrieve thinker's output to use as executor prompt
        std::string proposal_content;
        db_.query(
            "SELECT result FROM tasks WHERE conversation_id = ? AND task_type = 'think' "
            "ORDER BY id DESC LIMIT 1",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conversation_id);
            },
            [&](sqlite3_stmt* stmt) {
                auto r = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (r) proposal_content = r;
            }
        );

        if (proposal_content.empty()) {
            std::cerr << "ERROR: no thinker output found for conversation "
                      << conversation_id << "\n";
            return false;
        }

        // Create executor task
        auto session_id = generate_uuid();
        std::string prompt =
            "# Implementation Task\n\n" + proposal_content +
            "\n\n---\n\nImplement this plan. Follow the steps precisely. "
            "When done, produce a SUMMARY block with: status (success/partial/failed), "
            "files_changed, and notes.\n";

        db_.execute(
            "INSERT INTO tasks (agent, task_type, status, prompt, conversation_id, session_id) "
            "VALUES (?, 'execute', 'pending', ?, ?, ?)",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, executor_agent_.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, prompt.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 3, conversation_id);
                sqlite3_bind_text(stmt, 4, session_id.c_str(), -1, SQLITE_TRANSIENT);
            }
        );

        db_.update_conversation_state(conversation_id, "executing");
        std::cout << "[conversation " << conversation_id
                  << "] approved at human gate — dispatching executor\n";
        return true;
    }

private:
    Database& db_;
    std::string thinker_agent_ = "thinker";
    std::string executor_agent_ = "executor";
    std::string reviewer_agent_ = "reviewer";

    bool handle_thinking(int64_t conv_id, const ConversationRecord& conv, const ParsedOutput& parsed) {
        if (!parsed.proposals.empty()) {
            const auto& prop = parsed.proposals[0];

            if (conv.pipeline == "executor") {
                // Executor pipeline: THINKING → APPROVED (human gate)
                db_.update_conversation_state(conv_id, "approved");
                std::cout << "[conversation " << conv_id
                          << "] thinking -> approved (awaiting human gate)"
                          << " — proposal: " << prop.title << "\n";
                std::cout << "\n=== PROPOSAL FOR REVIEW ===\n"
                          << "Title: " << prop.title << "\n\n"
                          << prop.content << "\n"
                          << "===========================\n\n"
                          << "To approve: quorum_daemon --config <path> gate --approve --conversation "
                          << conv_id << "\n"
                          << "To reject:  quorum_daemon --config <path> gate --reject --conversation "
                          << conv_id << "\n\n";
                return false;  // pauses — human must approve via gate CLI
            }

            // Analyst pipeline (unchanged): THINKING → REVIEWING
            auto session_id = generate_uuid();
            std::string prompt =
                "# Proposal to Review\n\nTitle: " + prop.title +
                "\n\n" + prop.content +
                "\n\n---\n\nReview this proposal. Respond with a REVIEW block. "
                "Use verdict: approve, revise, reject, or escalate.\n";

            db_.execute(
                "INSERT INTO tasks (agent, task_type, status, prompt, conversation_id, session_id) "
                "VALUES (?, 'review', 'pending', ?, ?, ?)",
                [&](sqlite3_stmt* stmt) {
                    sqlite3_bind_text(stmt, 1, reviewer_agent_.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 2, prompt.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(stmt, 3, conv_id);
                    sqlite3_bind_text(stmt, 4, session_id.c_str(), -1, SQLITE_TRANSIENT);
                }
            );

            db_.update_conversation_state(conv_id, "reviewing");
            std::cout << "[conversation " << conv_id
                      << "] thinking -> reviewing — proposal: " << prop.title << "\n";
            return true;
        }

        // No proposals — close
        db_.update_conversation_state(conv_id, "closed");
        std::cout << "[conversation " << conv_id
                  << "] closed — thinker produced no proposals\n";
        return false;
    }

    bool handle_reviewing(int64_t conv_id, const ConversationRecord& conv, const ParsedOutput& parsed) {
        std::string verdict = "reject";
        std::string reasoning;
        if (!parsed.reviews.empty()) {
            verdict = parsed.reviews[0].verdict;
            reasoning = parsed.reviews[0].reasoning;
            // Lowercase the verdict
            for (auto& c : verdict) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        if (verdict == "approve") {
            // Phase 0.7: approved = done
            db_.complete_conversation(conv_id);
            std::cout << "[conversation " << conv_id
                      << "] approved — transitioning to DONE\n";
            return false;
        }

        if (verdict == "revise") {
            if (conv.round + 1 < conv.max_rounds) {
                db_.update_conversation_round(conv_id, conv.round + 1);

                // Retrieve the original Thinker's session_id
                std::string original_session_id;
                db_.query(
                    "SELECT session_id FROM tasks WHERE conversation_id = ? AND task_type = 'think' "
                    "ORDER BY id ASC LIMIT 1",
                    [&](sqlite3_stmt* stmt) {
                        sqlite3_bind_int64(stmt, 1, conv_id);
                    },
                    [&](sqlite3_stmt* stmt) {
                        auto s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                        if (s) original_session_id = s;
                    }
                );
                if (original_session_id.empty()) {
                    original_session_id = generate_uuid();
                }

                std::string prompt =
                    "# Revision Requested\n\nThe reviewer requested revisions:\n\n" +
                    reasoning +
                    "\n\n---\n\nRevise your proposal accordingly. "
                    "Produce an updated PROPOSAL block.\n";

                db_.execute(
                    "INSERT INTO tasks (agent, task_type, status, prompt, conversation_id, session_id) "
                    "VALUES (?, 'think', 'pending', ?, ?, ?)",
                    [&](sqlite3_stmt* stmt) {
                        sqlite3_bind_text(stmt, 1, thinker_agent_.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(stmt, 2, prompt.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_int64(stmt, 3, conv_id);
                        sqlite3_bind_text(stmt, 4, original_session_id.c_str(), -1, SQLITE_TRANSIENT);
                    }
                );

                db_.update_conversation_state(conv_id, "thinking");
                std::cout << "[conversation " << conv_id
                          << "] revise — round " << (conv.round + 1) << "\n";
                return true;
            }

            // Max rounds exhausted
            db_.update_conversation_state(conv_id, "closed");
            std::cout << "[conversation " << conv_id
                      << "] closed — max rounds exhausted\n";
            return false;
        }

        if (verdict == "escalate") {
            db_.pause_conversation(conv_id, "agent escalation: " + reasoning);
            std::cout << "[conversation " << conv_id
                      << "] paused — agent requested escalation\n";
            return false;
        }

        // reject or anything else
        db_.update_conversation_state(conv_id, "closed");
        std::cout << "[conversation " << conv_id
                  << "] closed — verdict: " << verdict << "\n";
        return false;
    }

    bool handle_executing(int64_t conv_id, const ConversationRecord& conv,
                          const ParsedOutput& parsed) {
        // Executor done → create reviewer task with executor's output context

        // Get executor's result text (most recent execute task)
        std::string executor_output;
        db_.query(
            "SELECT result FROM tasks WHERE conversation_id = ? AND task_type = 'execute' "
            "ORDER BY id DESC LIMIT 1",
            [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, conv_id); },
            [&](sqlite3_stmt* stmt) {
                auto r = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (r) executor_output = r;
            }
        );

        // Get original thinker proposal (first think task's result)
        std::string thinker_proposal;
        db_.query(
            "SELECT result FROM tasks WHERE conversation_id = ? AND task_type = 'think' "
            "ORDER BY id ASC LIMIT 1",
            [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, conv_id); },
            [&](sqlite3_stmt* stmt) {
                auto r = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (r) thinker_proposal = r;
            }
        );

        // Build reviewer prompt with executor context
        auto session_id = generate_uuid();
        std::string prompt =
            "# Code Review\n\n"
            "An executor agent implemented the following plan. Review the changes.\n\n"
            "## Original Plan\n\n" + thinker_proposal +
            "\n\n## Executor Output\n\n" + executor_output +
            "\n\n---\n\nReview the executor's work. Verify correctness and completeness. "
            "Respond with a REVIEW block. Use verdict: approve or reject with reason.\n";

        db_.execute(
            "INSERT INTO tasks (agent, task_type, status, prompt, conversation_id, session_id) "
            "VALUES (?, 'review', 'pending', ?, ?, ?)",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, reviewer_agent_.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, prompt.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 3, conv_id);
                sqlite3_bind_text(stmt, 4, session_id.c_str(), -1, SQLITE_TRANSIENT);
            }
        );

        db_.update_conversation_state(conv_id, "reviewing");
        std::cout << "[conversation " << conv_id << "] executing -> reviewing\n";
        return true;
    }

    // ── Pause condition helpers ──────────────────────────────────────────────

    int64_t get_median_input_tokens(int64_t conv_id) {
        std::vector<int64_t> tokens;
        db_.query(
            "SELECT token_in FROM tasks WHERE conversation_id = ? AND token_in IS NOT NULL ORDER BY token_in",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conv_id);
            },
            [&](sqlite3_stmt* stmt) {
                tokens.push_back(sqlite3_column_int64(stmt, 0));
            }
        );
        if (tokens.empty()) return 0;
        return tokens[tokens.size() / 2];
    }

    int count_recent_failures(int64_t conv_id, int n) {
        int count = 0;
        db_.query(
            "SELECT status FROM tasks WHERE conversation_id = ? ORDER BY id DESC LIMIT ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conv_id);
                sqlite3_bind_int(stmt, 2, n);
            },
            [&](sqlite3_stmt* stmt) {
                auto s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (s && std::string(s) == "failed") ++count;
            }
        );
        return count;
    }

    int64_t get_task_token_in(int64_t task_id) {
        int64_t tokens = 0;
        db_.query(
            "SELECT token_in FROM tasks WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, task_id);
            },
            [&](sqlite3_stmt* stmt) {
                if (sqlite3_column_type(stmt, 0) != SQLITE_NULL)
                    tokens = sqlite3_column_int64(stmt, 0);
            }
        );
        return tokens;
    }

    PauseCheck check_pause_conditions(int64_t conv_id, int64_t task_id, double task_cost) {
        auto conv = db_.get_conversation(conv_id);
        if (!conv) return {true, "conversation not found"};

        // 1. Budget exceeded (post-spend check)
        if (conv->spent_usd >= conv->budget_usd) {
            return {true, "budget exceeded: $" + std::to_string(conv->spent_usd)
                         + " >= $" + std::to_string(conv->budget_usd)};
        }

        // 2. Token anomaly: current task's token_in > 2x median
        auto current_tokens = get_task_token_in(task_id);
        auto median = get_median_input_tokens(conv_id);
        if (median > 0 && current_tokens > median * 2) {
            return {true, "token anomaly: " + std::to_string(current_tokens)
                         + " tokens (median: " + std::to_string(median) + ")"};
        }

        // 3. Consecutive failures (2+)
        auto failures = count_recent_failures(conv_id, 2);
        if (failures >= 2) {
            return {true, "consecutive failures: " + std::to_string(failures)};
        }

        return {false, ""};
    }
};

} // namespace sui::quorum
