#pragma once

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "utils/config.h"
#include "utils/uuid.h"
#include "storage/database.h"
#include "agent/output_parser.h"
#include "agent/context_assembler.h"

namespace sui::quorum {

enum class ConvState { active, waiting_for_human, done, closed, paused };

class ConversationEngine {
public:
    ConversationEngine(Database& db, const ConversationConfig& cfg,
                       const std::vector<AgentMetadata>& agents,
                       const ContextAssembler* assembler = nullptr)
        : db_(db), cfg_(cfg), agents_(agents), assembler_(assembler) {}

    // Start a new conversation. Creates the first task for the leader agent.
    // Returns conversation ID.
    int64_t start(const std::string& goal, double budget_usd = 5.0,
                  int max_rounds = 20) {
        auto conv_id = db_.create_conversation(goal, budget_usd, max_rounds);

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

        // 4. Append knowledge entries to ledger
        for (const auto& k : parsed.knowledge) {
            db_.append_knowledge(conv_id, agent_id, conv->round, k.topic, k.content);
        }

        // 5. Determine next agent
        std::string next_agent;
        std::string next_prompt;
        bool is_done = false;
        bool is_human = false;

        if (parsed.handoff.has_value()) {
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

        // 7. Route: done -> complete; human -> waiting_for_human; budget/turns -> pause
        if (is_done) {
            db_.complete_conversation(conv_id);
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

        // Budget check
        if (conv->spent_usd >= conv->budget_usd) {
            db_.pause_conversation(conv_id, "budget exceeded ($"
                + std::to_string(conv->spent_usd) + "/$"
                + std::to_string(conv->budget_usd) + ")");
            std::cout << "[conversation " << conv_id
                      << "] paused -- budget exceeded\n";
            return false;
        }

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
            next_prompt = "Continue the conversation. Review knowledge ledger and decide next steps.";
        }

        create_task(conv_id, next_agent, "turn", next_prompt, session_id);
        update_current_agent(conv_id, next_agent);
        increment_turn(conv_id);

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

private:
    Database& db_;
    ConversationConfig cfg_;
    std::vector<AgentMetadata> agents_;
    const ContextAssembler* assembler_ = nullptr;

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

        if (assembler_) {
            // Build roster
            auto roster = ContextAssembler::build_roster(agents_, agent, cfg_);

            // Find agent metadata for vault path + skill file
            std::string vault_dir;
            std::string skill_file;
            for (const auto& a : agents_) {
                if (a.id == agent) {
                    vault_dir = a.vault_path;
                    skill_file = a.skill_file;
                    break;
                }
            }

            // Assemble full prompt with vault context + roster + task
            if (!vault_dir.empty()) {
                final_prompt = assembler_->assemble(agent, vault_dir, task_type, prompt, roster, skill_file);
            } else {
                // No vault -- just roster + task
                final_prompt = roster + "\n---\n\n# Current Task\n\n" + prompt + "\n";
            }
        }

        db_.execute(
            "INSERT INTO tasks (agent, task_type, prompt, conversation_id, session_id) "
            "VALUES (?, ?, ?, ?, ?)",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, agent.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, task_type.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, final_prompt.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 4, conv_id);
                sqlite3_bind_text(stmt, 5, session_id.c_str(), -1, SQLITE_TRANSIENT);
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
