#pragma once

#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <iomanip>

#include "storage/database.h"
#include "agent/output_parser.h"

namespace sui::quorum {

enum class ConvState {
    init, thinking, reviewing, approved, executing, evaluating, done, closed, paused
};

class ConversationEngine {
public:
    explicit ConversationEngine(Database& db) : db_(db) {}

    // Start a new conversation. Returns conversation ID.
    int64_t start(const std::string& goal, double budget_usd = 5.0, int max_rounds = 3) {
        auto conv_id = db_.create_conversation(goal, budget_usd, max_rounds);

        auto session_id = generate_session_id();

        std::string prompt =
            "# Goal\n\n" + goal +
            "\n\n---\n\nAnalyze this goal. Produce a PROPOSAL block with your recommended approach.\n";

        db_.execute(
            "INSERT INTO tasks (agent, task_type, status, prompt, conversation_id, session_id) "
            "VALUES ('thinker', 'think', 'pending', ?, ?, ?)",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, prompt.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 2, conv_id);
                sqlite3_bind_text(stmt, 3, session_id.c_str(), -1, SQLITE_TRANSIENT);
            }
        );

        db_.update_conversation_state(conv_id, "thinking");

        auto display = goal.size() > 60 ? goal.substr(0, 60) : goal;
        std::cout << "[conversation " << conv_id << "] started — " << display << "\n";

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

        // Re-read for budget check
        conv = db_.get_conversation(conv_id);
        if (!conv) return false;
        if (conv->spent_usd >= conv->budget_usd) {
            std::string reason = "budget exceeded: $" + std::to_string(conv->spent_usd);
            db_.pause_conversation(conv_id, reason);
            std::cout << "[conversation " << conv_id << "] paused — " << reason << "\n";
            return false;
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
            // Phase 0.9 stub
            db_.complete_conversation(conv_id);
            return false;
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
            auto session_id = last_session_id.empty() ? generate_session_id() : last_session_id;
            db_.execute(
                "INSERT INTO tasks (agent, task_type, status, prompt, conversation_id, session_id) "
                "VALUES ('thinker', 'think', 'pending', ?, ?, ?)",
                [&](sqlite3_stmt* stmt) {
                    sqlite3_bind_text(stmt, 1, last_prompt.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(stmt, 2, conversation_id);
                    sqlite3_bind_text(stmt, 3, session_id.c_str(), -1, SQLITE_TRANSIENT);
                }
            );
        } else if (last_task_type == "review") {
            db_.update_conversation_state(conversation_id, "reviewing");
            auto session_id = generate_session_id();
            db_.execute(
                "INSERT INTO tasks (agent, task_type, status, prompt, conversation_id, session_id) "
                "VALUES ('reviewer', 'review', 'pending', ?, ?, ?)",
                [&](sqlite3_stmt* stmt) {
                    sqlite3_bind_text(stmt, 1, last_prompt.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(stmt, 2, conversation_id);
                    sqlite3_bind_text(stmt, 3, session_id.c_str(), -1, SQLITE_TRANSIENT);
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

private:
    Database& db_;

    bool handle_thinking(int64_t conv_id, const ConversationRecord& conv, const ParsedOutput& parsed) {
        if (!parsed.proposals.empty()) {
            const auto& prop = parsed.proposals[0];

            // Create reviewer task with fresh session_id
            auto session_id = generate_session_id();
            std::string prompt =
                "# Proposal to Review\n\nTitle: " + prop.title +
                "\n\n" + prop.content +
                "\n\n---\n\nReview this proposal. Respond with a REVIEW block. "
                "Use verdict: approve, revise, or reject.\n";

            db_.execute(
                "INSERT INTO tasks (agent, task_type, status, prompt, conversation_id, session_id) "
                "VALUES ('reviewer', 'review', 'pending', ?, ?, ?)",
                [&](sqlite3_stmt* stmt) {
                    sqlite3_bind_text(stmt, 1, prompt.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(stmt, 2, conv_id);
                    sqlite3_bind_text(stmt, 3, session_id.c_str(), -1, SQLITE_TRANSIENT);
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
                    original_session_id = generate_session_id();
                }

                std::string prompt =
                    "# Revision Requested\n\nThe reviewer requested revisions:\n\n" +
                    reasoning +
                    "\n\n---\n\nRevise your proposal accordingly. "
                    "Produce an updated PROPOSAL block.\n";

                db_.execute(
                    "INSERT INTO tasks (agent, task_type, status, prompt, conversation_id, session_id) "
                    "VALUES ('thinker', 'think', 'pending', ?, ?, ?)",
                    [&](sqlite3_stmt* stmt) {
                        sqlite3_bind_text(stmt, 1, prompt.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_int64(stmt, 2, conv_id);
                        sqlite3_bind_text(stmt, 3, original_session_id.c_str(), -1, SQLITE_TRANSIENT);
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

        // reject or anything else
        db_.update_conversation_state(conv_id, "closed");
        std::cout << "[conversation " << conv_id
                  << "] closed — verdict: " << verdict << "\n";
        return false;
    }

    static std::string generate_session_id() {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

        auto r = [&]() { return dist(gen); };

        std::ostringstream ss;
        ss << std::hex << std::setfill('0');
        uint32_t a = r(), b = r(), c = r(), d = r();
        ss << std::setw(8) << a << "-"
           << std::setw(4) << (b >> 16) << "-"
           << std::setw(4) << ((b & 0x0FFF) | 0x4000) << "-"  // version 4
           << std::setw(4) << (((c >> 16) & 0x3FFF) | 0x8000) << "-"  // variant 1
           << std::setw(4) << (c & 0xFFFF)
           << std::setw(8) << d;
        return ss.str();
    }
};

} // namespace sui::quorum
