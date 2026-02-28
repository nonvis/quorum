#pragma once

#include <optional>
#include <string>
#include <string_view>
#include "utils/http_client.h"
#include "utils/json.h"

namespace sui::quorum {

static constexpr std::string_view ANTHROPIC_API_VERSION = "2023-06-01";

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

        auto resp = http_.post_json(api_url, body, {
            "x-api-key: " + api_key,
            "anthropic-version: " + std::string(ANTHROPIC_API_VERSION),
        });
        if (!resp.success()) {
            return {.success = false, .error = resp.error.empty() ? resp.body : resp.error};
        }

        auto content = json::extract_anthropic_content(resp.body);
        auto input_tokens = json::extract_int(resp.body, "input_tokens");
        auto output_tokens = json::extract_int(resp.body, "output_tokens");

        return {
            .success = true,
            .output = content.value_or(""),
            .error = {},
            .tokens_used = static_cast<int>(input_tokens + output_tokens),
        };
    }

private:
    HttpClient& http_;
};

} // namespace sui::quorum
