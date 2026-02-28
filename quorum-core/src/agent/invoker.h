#pragma once

#include <optional>
#include <string>
#include "utils/http_client.h"
#include "utils/json.h"

namespace sui::quorum {

struct InvocationResult {
    bool success{false};
    std::string output;
    std::string error;
    int tokens_used{0};
};

// LLM invoker — calls external APIs (Claude, Ollama)
// Triggered by the daemon but runs as an external API call.
class Invoker {
public:
    explicit Invoker(HttpClient& http) : http_(http) {}

    [[nodiscard]] InvocationResult invoke_frontier(
        const std::string& api_url,
        const std::string& api_key,
        const std::string& model,
        const std::string& system_prompt,
        const std::string& user_prompt,
        uint64_t max_tokens) {

        auto body = json::build_object({
            {"model", json::quote(model)},
            {"max_tokens", std::to_string(max_tokens)},
            {"messages", "[{\"role\":\"user\",\"content\":" + json::quote(user_prompt) + "}]"},
            {"system", json::quote(system_prompt)},
        });

        // Set auth header via a temporary post
        auto resp = http_.post_json(api_url, body);
        if (!resp.success()) {
            return {.success = false, .error = resp.error.empty() ? resp.body : resp.error};
        }

        auto content = json::extract_string(resp.body, "text");
        return {
            .success = true,
            .output = content.value_or(""),
        };
    }

private:
    HttpClient& http_;
};

} // namespace sui::quorum
