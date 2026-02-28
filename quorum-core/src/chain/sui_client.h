#pragma once

#include <optional>
#include <string>
#include "utils/http_client.h"
#include "utils/json.h"

namespace sui::quorum {

// Sui JSON-RPC client
class SuiClient {
public:
    SuiClient(HttpClient& http, const std::string& rpc_url)
        : http_(http), rpc_url_(rpc_url) {}

    [[nodiscard]] std::optional<std::string> get_object(const std::string& object_id) {
        auto body = json::build_object({
            {"jsonrpc", json::quote("2.0")},
            {"id", "1"},
            {"method", json::quote("sui_getObject")},
            {"params", "[" + json::quote(object_id) + ",{\"showContent\":true}]"},
        });
        auto resp = http_.post_json(rpc_url_, body);
        if (!resp.success()) return std::nullopt;
        return resp.body;
    }

private:
    HttpClient& http_;
    std::string rpc_url_;
};

} // namespace sui::quorum
