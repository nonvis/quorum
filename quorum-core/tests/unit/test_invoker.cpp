// tests/unit/test_invoker.cpp
// Unit tests for Invoker's envelope validation + parsing.
// Tests the static validate_claude_output() / parse_envelope() helpers, which
// are the exact code invoke() runs, without needing a DB or a subprocess.
// Run:  cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#include "agent/invoker.h"

using sui::quorum::CommandResult;
using sui::quorum::Invoker;

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        std::exit(1);
    }
    std::cout << "[PASS] " << msg << "\n";
}

// --- Test 1: Non-zero exit code ------------------------------------------------

static void test_nonzero_exit_code_fails() {
    CommandResult cr{
        .output = "Error: API key not set",
        .exit_code = 1,
    };
    auto err = Invoker::validate_claude_output(cr);
    check(err.has_value(), "non-zero exit code returns error");
    check(err->find("non-zero exit code") != std::string::npos,
          "error mentions non-zero exit code");
    check(err->find("API key not set") != std::string::npos,
          "error includes original output");
}

static void test_nonzero_exit_code_empty_output() {
    CommandResult cr{.output = "", .exit_code = 2};
    auto err = Invoker::validate_claude_output(cr);
    check(err.has_value(), "non-zero exit with empty output returns error");
}

// --- Test 2: Exit code 0 but non-JSON output -----------------------------------

static void test_exit_zero_plain_text_fails() {
    CommandResult cr{
        .output = "Something went wrong but process exited 0",
        .exit_code = 0,
    };
    auto err = Invoker::validate_claude_output(cr);
    check(err.has_value(), "plain text with exit 0 returns error");
    check(err->find("invalid JSON") != std::string::npos,
          "error mentions invalid JSON");
}

static void test_exit_zero_empty_output_fails() {
    CommandResult cr{.output = "", .exit_code = 0};
    auto err = Invoker::validate_claude_output(cr);
    check(err.has_value(), "empty output with exit 0 returns error");
}

static void test_exit_zero_wrong_type_fails() {
    // JSON but wrong "type" value
    CommandResult cr{
        .output = R"({"type":"error","error":"rate_limited"})",
        .exit_code = 0,
    };
    auto err = Invoker::validate_claude_output(cr);
    check(err.has_value(), "JSON with type!=result returns error");
}

static void test_exit_zero_no_type_field_fails() {
    // Valid JSON but no "type" key
    CommandResult cr{
        .output = R"({"message":"hello","status":"ok"})",
        .exit_code = 0,
    };
    auto err = Invoker::validate_claude_output(cr);
    check(err.has_value(), "JSON without type field returns error");
}

// --- Test 3: Valid claude -p output (July 2026 shape) ---------------------------

static void test_valid_claude_output_passes() {
    CommandResult cr{
        .output = R"({"type":"result","subtype":"success","result":"Hello world","total_cost_usd":0.03,"usage":{"input_tokens":100,"output_tokens":50}})",
        .exit_code = 0,
    };
    auto err = Invoker::validate_claude_output(cr);
    check(!err.has_value(), "valid claude -p JSON passes validation");
}

static void test_valid_claude_output_with_is_error_passes() {
    // Full realistic output
    CommandResult cr{
        .output = R"({"type":"result","subtype":"success","is_error":false,"session_id":"abc123","result":"The answer is 42.","total_cost_usd":0.029,"duration_ms":1500,"usage":{"input_tokens":2,"output_tokens":18}})",
        .exit_code = 0,
    };
    auto err = Invoker::validate_claude_output(cr);
    check(!err.has_value(), "full realistic claude -p output passes validation");

    // The old shape is a subset of the new one — parse_envelope reads it too.
    auto parsed = Invoker::parse_envelope(cr.output);
    check(parsed.success, "old-shape envelope parses");
    check(parsed.output == "The answer is 42.", "old-shape result text");
    check(parsed.session_id == "abc123", "old-shape session_id");
    check(parsed.tokens_in == 2 && parsed.tokens_out == 18, "old-shape usage tokens");
    check(std::fabs(parsed.cost - 0.029) < 1e-9, "old-shape total_cost_usd");
}

// --- Test 4: the REAL Claude Code 2.1.261 envelope ------------------------------
//
// Captured verbatim on 2026-09-04 from
//   env -u CLAUDECODE claude -p --output-format json   (prompt: "reply OK")
// 2040 bytes (2041 on disk, minus the trailing newline). Its top-level
// "type":"result" is the 18th key, while
// usage.iterations[0]."type" == "message" sits ~1180 bytes earlier — which is
// exactly what made the flat by-key scan reject every healthy reply
// ("claude -p returned invalid JSON for task 1").
static const std::string kEnvelope_2_1_261 =
    R"JSON({"duration_api_ms":2797,"stop_reason":"end_turn","session_id":"ef0e69d4-9d90-4bf1-ad93-0c21b1f5ec2b","total_cost_usd":0.0057465,"usage":{"input_tokens":2,"cache_creation_input_tokens":0,"cache_read_input_tokens":18338,"output_tokens":4,"output_tokens_details":{"thinking_tokens":0},"server_tool_use":{"web_search_requests":0,"web_fetch_requests":0},"service_tier":"standard","cache_creation":{"ephemeral_1h_input_tokens":0,"ephemeral_5m_input_tokens":0},"inference_geo":"not_available","iterations":[{"input_tokens":2,"output_tokens":4,"cache_read_input_tokens":18338,"cache_creation_input_tokens":0,"cache_creation":{"ephemeral_5m_input_tokens":0,"ephemeral_1h_input_tokens":0},"type":"message"}],"speed":"standard"},"modelUsage":{"claude-haiku-4-5-20251001":{"inputTokens":897,"outputTokens":9,"cacheReadInputTokens":0,"cacheCreationInputTokens":0,"webSearchRequests":0,"costUSD":0.000942,"contextWindow":200000,"maxOutputTokens":32000,"thinkingTokens":0,"canonicalModel":"claude-haiku-4-5","provider":"firstParty","costBasis":"list"},"claude-fable-5-1":{"inputTokens":2,"outputTokens":4,"cacheReadInputTokens":18338,"cacheCreationInputTokens":0,"webSearchRequests":0,"costUSD":0.0048045,"contextWindow":1000000,"maxOutputTokens":64000,"thinkingTokens":0,"canonicalModel":"claude-fable-5-1","provider":"firstParty","costBasis":"list"}},"permission_denials":[],"terminal_reason":"completed","fast_mode_state":"off","fast_mode_disabled_reason":"sdk_opt_in_required","subagent_stats":{"spawned":0,"requested":{"background":0,"foreground":0,"unset":0},"started_in_background":0,"max_depth":0,"spawned_by_subagents":0,"completed":0,"failed":0,"killed":{"parent":0,"user":0,"system":0},"refused":{"depth_limit":0,"concurrency_limit":0,"budget":0},"by_type":{}},"is_error":false,"num_turns":1,"subtype":"success","api_error_status":null,"result":"OK","ttft_ms":2030,"type":"result","duration_ms":2073,"uuid":"46e0480e-9c79-4874-8c90-663d6c013084","ttft_stream_ms":1160,"time_to_request_ms":21,"first_content_frame_ms":1160,"queued_turn_count":0})JSON";

static void test_real_2_1_261_envelope_validates() {
    CommandResult cr{.output = kEnvelope_2_1_261, .exit_code = 0};
    auto err = Invoker::validate_claude_output(cr);
    check(!err.has_value(),
          "REAL 2.1.261 envelope passes validation (was: 'invalid JSON')");
}

static void test_real_2_1_261_envelope_parses() {
    auto p = Invoker::parse_envelope(kEnvelope_2_1_261);
    check(p.success, "2.1.261: parse_envelope succeeds");
    check(p.error.empty(), "2.1.261: no error text");
    check(p.output == "OK", "2.1.261: output == 'OK'");
    check(p.session_id == "ef0e69d4-9d90-4bf1-ad93-0c21b1f5ec2b",
          "2.1.261: session_id from the envelope");
    check(p.tokens_in == 2, "2.1.261: tokens_in == usage.input_tokens == 2");
    check(p.tokens_out == 4, "2.1.261: tokens_out == usage.output_tokens == 4");
    check(p.cache_creation_tokens == 0,
          "2.1.261: cache_creation == usage.cache_creation_input_tokens == 0");
    check(p.cache_read_tokens == 18338,
          "2.1.261: cache_read == usage.cache_read_input_tokens == 18338");
    check(std::fabs(p.cost - 0.0057465) < 1e-12,
          "2.1.261: cost == total_cost_usd == 0.0057465");
}

// The real envelope's iterations happen to equal its usage totals, so this
// synthetic 2.1.261-shaped envelope makes the scoping visible: iterations[]
// precede the totals inside "usage", and modelUsage repeats them again.
static void test_usage_totals_not_iteration_values() {
    const std::string env =
        R"({"session_id":"sid-9","total_cost_usd":0.25,)"
        R"("usage":{"iterations":[)"
        R"({"input_tokens":7,"output_tokens":1,"cache_creation_input_tokens":11,"cache_read_input_tokens":13,"type":"message"},)"
        R"({"input_tokens":9,"output_tokens":3,"cache_creation_input_tokens":17,"cache_read_input_tokens":19,"type":"message"}],)"
        R"("input_tokens":100,"output_tokens":200,)"
        R"("cache_creation_input_tokens":300,"cache_read_input_tokens":400},)"
        R"("modelUsage":{"claude-x":{"inputTokens":55,"outputTokens":66}},)"
        R"("is_error":false,"subtype":"success","result":"multi","type":"result"})";

    auto p = Invoker::parse_envelope(env);
    check(p.success, "multi-iteration envelope parses");
    check(p.output == "multi", "multi-iteration: result text");
    check(p.tokens_in == 100, "multi-iteration: tokens_in is the usage TOTAL, not iterations[0]");
    check(p.tokens_out == 200, "multi-iteration: tokens_out is the usage TOTAL");
    check(p.cache_creation_tokens == 300, "multi-iteration: cache_creation is the usage TOTAL");
    check(p.cache_read_tokens == 400, "multi-iteration: cache_read is the usage TOTAL");
}

// --- Test 5: failure envelopes --------------------------------------------------

static void test_type_error_envelope_still_fails() {
    auto p = Invoker::parse_envelope(R"({"type":"error","error":"rate_limited"})");
    check(!p.success, "type:'error' envelope fails to parse");
    check(p.error.find("invalid JSON structure") != std::string::npos,
          "type:'error' error names the structure problem");
}

static void test_is_error_true_envelope_fails_with_result_text() {
    const std::string env =
        R"({"type":"result","subtype":"error_during_execution","is_error":true,)"
        R"("session_id":"sid-err","result":"Model refused: rate limited",)"
        R"("usage":{"input_tokens":1,"output_tokens":0}})";

    auto p = Invoker::parse_envelope(env);
    check(!p.success, "is_error:true envelope fails even though type=='result'");
    check(p.error.find("Model refused: rate limited") != std::string::npos,
          "is_error:true error carries the result text");
    check(p.error.find("error_during_execution") != std::string::npos,
          "is_error:true error carries the subtype");

    CommandResult cr{.output = env, .exit_code = 0};
    auto err = Invoker::validate_claude_output(cr);
    check(err.has_value(), "is_error:true also fails validate_claude_output");
}

// --- main ----------------------------------------------------------------------

int main() {
    std::cout << "=== test_invoker (agent/invoker.h validation) ===\n\n";

    test_nonzero_exit_code_fails();
    test_nonzero_exit_code_empty_output();
    test_exit_zero_plain_text_fails();
    test_exit_zero_empty_output_fails();
    test_exit_zero_wrong_type_fails();
    test_exit_zero_no_type_field_fails();
    test_valid_claude_output_passes();
    test_valid_claude_output_with_is_error_passes();
    test_real_2_1_261_envelope_validates();
    test_real_2_1_261_envelope_parses();
    test_usage_totals_not_iteration_values();
    test_type_error_envelope_still_fails();
    test_is_error_true_envelope_fails_with_result_text();

    std::cout << "\nAll tests passed.\n";
    return 0;
}
