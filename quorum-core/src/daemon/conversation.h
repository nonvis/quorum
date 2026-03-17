#pragma once

#include <iostream>
#include <string>

#include "utils/uuid.h"
#include "storage/database.h"
#include "agent/output_parser.h"

namespace sui::quorum {

enum class ConvState { active, waiting_for_human, done, closed, paused };

class ConversationEngine {
public:
    explicit ConversationEngine(Database& db) : db_(db) {}

    // Start a new conversation. Returns conversation ID.
    int64_t start(const std::string& goal, double budget_usd = 5.0,
                  int max_rounds = 3) {
        auto conv_id = db_.create_conversation(goal, budget_usd, max_rounds);

        auto display = goal.size() > 60 ? goal.substr(0, 60) : goal;
        std::cout << "[conversation " << conv_id << "] started"
                  << " — " << display << "\n";

        return conv_id;
    }

    // Called after a task completes. Routes to next state.
    // Stub — will be reimplemented for team mode in task #3.
    bool on_task_complete(int64_t /*task_id*/, const ParsedOutput& /*parsed*/,
                          double /*task_cost*/) {
        return false;
    }

    // Resume a paused conversation.
    // Stub — will be reimplemented for team mode.
    bool resume(int64_t conversation_id) {
        auto conv = db_.get_conversation(conversation_id);
        if (!conv || conv->state != "paused") return false;

        db_.update_conversation_state(conversation_id, "active");
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
        std::cout << "[conversation " << conversation_id
                  << "] closed by operator\n";
    }

private:
    Database& db_;
};

} // namespace sui::quorum
