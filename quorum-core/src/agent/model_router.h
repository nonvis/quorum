#pragma once

#include <cstdint>
#include <string>

namespace sui::quorum {

// Cost tiers for inference routing
enum class InferenceTier : uint8_t {
    RuleBased  = 0,  // Free — all daemon logic
    LocalLLM   = 1,  // $0 — Ollama / llama.cpp
    Frontier   = 2,  // $$ — Claude API
    Human      = 3,  // Priceless — capital decisions
};

// Selects inference tier based on task type (never on LLM output)
class ModelRouter {
public:
    [[nodiscard]] InferenceTier route(const std::string& task_type) const {
        if (task_type == "routine_scan" || task_type == "status_check")
            return InferenceTier::LocalLLM;
        if (task_type == "deep_analysis" || task_type == "proposal_review")
            return InferenceTier::Frontier;
        if (task_type == "capital_decision" || task_type == "deadlock")
            return InferenceTier::Human;
        return InferenceTier::LocalLLM;
    }
};

} // namespace sui::quorum
