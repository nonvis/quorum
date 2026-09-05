// tests/unit/test_invoker_cache_fields.cpp
// Phase 7 Track 5 — the two prompt-cache token fields, read the way the daemon
// reads them.
//
// The invoker does NOT scan the envelope flat. Invoker::parse_envelope() (the
// exact function invoke() calls at Layer 2) pulls the top-level "usage" object
// out first and then reads DEPTH-0 keys inside it:
//   "usage": { "input_tokens", "output_tokens",
//              "cache_creation_input_tokens", "cache_read_input_tokens",
//              "iterations": [ { ...the same four keys, PER ROUND-TRIP... } ] }
// A flat by-key extractor hits usage.iterations[0] first and reports ONE
// round-trip instead of the turn total. The fixture below is built so the two
// answers cannot coincide: usage totals are 100/200/300/400, the iteration
// carries 7/1/11/13, and "modelUsage" repeats a third set again.
//
// (Until 2026-09-04 this file called json::extract_int on a nesting-free
// fixture and its header claimed that was how the invoker read tokens. It was
// not — that is the drift this rewrite closes.)
//
// Run:  cd build && ctest -R test_invoker_cache_fields --output-on-failure

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "agent/invoker.h"

using sui::quorum::Invoker;

static int g_failed = 0;

static void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        ++g_failed;
    } else {
        std::cout << "[PASS] " << msg << "\n";
    }
}

// Claude Code 2.1.261 envelope shape, trimmed to the accounting keys. Note the
// iterations[] array sits BEFORE the usage totals — that ordering is what a
// flat scan trips over, so it is preserved here deliberately.
static const std::string kFixture = R"({
    "type": "result",
    "subtype": "success",
    "is_error": false,
    "result": "pong",
    "session_id": "sid-123",
    "total_cost_usd": 0.0042,
    "usage": {
        "iterations": [
            {
                "input_tokens": 7,
                "output_tokens": 1,
                "cache_creation_input_tokens": 11,
                "cache_read_input_tokens": 13,
                "type": "message"
            }
        ],
        "input_tokens": 100,
        "output_tokens": 200,
        "cache_creation_input_tokens": 300,
        "cache_read_input_tokens": 400
    },
    "modelUsage": {
        "claude-x": {
            "inputTokens": 55,
            "outputTokens": 66,
            "cacheCreationInputTokens": 77,
            "cacheReadInputTokens": 88
        }
    }
})";

int main() {
    std::cout << "=====================================================\n";
    std::cout << "  Phase 7 Track 5 — cache-field extraction\n";
    std::cout << "=====================================================\n";

    auto r = Invoker::parse_envelope(kFixture);

    check(r.success, "envelope parses (parse_envelope, the Layer 2 function)");

    // The two cache fields are the subject of this test.
    check(r.cache_creation_tokens == 300,
          "cache_creation_tokens = usage TOTAL 300, not iterations[0] 11 (got " +
          std::to_string(r.cache_creation_tokens) + ")");
    check(r.cache_read_tokens == 400,
          "cache_read_tokens = usage TOTAL 400, not iterations[0] 13 (got " +
          std::to_string(r.cache_read_tokens) + ")");

    // The plain token pair travels the same path; a scoping bug would move all
    // four together, so assert them too.
    check(r.tokens_in == 100,
          "tokens_in = usage TOTAL 100, not iterations[0] 7 (got " +
          std::to_string(r.tokens_in) + ")");
    check(r.tokens_out == 200,
          "tokens_out = usage TOTAL 200, not iterations[0] 1 (got " +
          std::to_string(r.tokens_out) + ")");

    // (The old file also asserted cache_creation != cache_read. Dropped: with
    // the two totals fixed at 300 and 400 it is strictly implied by the two
    // checks above and can never fail while they pass — a gate that cannot go
    // red is not a gate.)

    return g_failed == 0 ? 0 : 1;
}
