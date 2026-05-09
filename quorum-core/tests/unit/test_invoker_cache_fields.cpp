// tests/unit/test_invoker_cache_fields.cpp
// Phase 7 Track 5 — verify json::extract_int reads all four token-accounting
// fields from a fixture JSON shaped like the real claude -p output.
//
// claude -p --output-format json returns a result envelope whose "usage"
// object is the standard Anthropic Messages API shape:
//   "usage": { "input_tokens": ..., "output_tokens": ...,
//              "cache_creation_input_tokens": ...,
//              "cache_read_input_tokens": ... }
// The Track 5 invoker pulls all four. This test feeds a fixture that
// matches the real shape and confirms each field round-trips correctly,
// which is the contract the live invoker depends on.
//
// Run:  cd build && ctest -R test_invoker_cache_fields --output-on-failure

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "utils/json.h"

static int g_failed = 0;

static void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        ++g_failed;
    } else {
        std::cout << "[PASS] " << msg << "\n";
    }
}

int main() {
    std::cout << "=====================================================\n";
    std::cout << "  Phase 7 Track 5 — cache-field extraction\n";
    std::cout << "=====================================================\n";

    // Fixture mirroring the real claude -p JSON envelope. All four token
    // fields populated with distinguishable values so a single-field bug
    // shows up in the assertions.
    const std::string fixture = R"({
        "type": "result",
        "subtype": "success",
        "is_error": false,
        "result": "pong",
        "session_id": "sid-123",
        "total_cost_usd": 0.0042,
        "usage": {
            "input_tokens": 11,
            "output_tokens": 22,
            "cache_creation_input_tokens": 3300,
            "cache_read_input_tokens": 4400
        }
    })";

    int64_t input_tokens =
        sui::quorum::json::extract_int(fixture, "input_tokens");
    int64_t output_tokens =
        sui::quorum::json::extract_int(fixture, "output_tokens");
    int64_t cache_creation =
        sui::quorum::json::extract_int(fixture, "cache_creation_input_tokens");
    int64_t cache_read =
        sui::quorum::json::extract_int(fixture, "cache_read_input_tokens");

    check(input_tokens == 11,
          "input_tokens extracted = 11 (got " + std::to_string(input_tokens) + ")");
    check(output_tokens == 22,
          "output_tokens extracted = 22 (got " + std::to_string(output_tokens) + ")");
    check(cache_creation == 3300,
          "cache_creation_input_tokens extracted = 3300 (got " +
          std::to_string(cache_creation) + ")");
    check(cache_read == 4400,
          "cache_read_input_tokens extracted = 4400 (got " +
          std::to_string(cache_read) + ")");

    // Also confirm the by-key extractor doesn't confuse cache_creation with
    // cache_read when both keys live in the same object.
    check(cache_creation != cache_read,
          "cache_creation and cache_read are read independently");

    return g_failed == 0 ? 0 : 1;
}
