// tests/unit/test_json.cpp
// Unit tests for utils/json.h extract functions — verifies key-vs-value disambiguation.
// Run:  cd build && ctest --output-on-failure  (or ./test_json)

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#include "utils/json.h"

namespace json = sui::quorum::json;

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        std::exit(1);
    }
    std::cout << "[PASS] " << msg << "\n";
}

// ─── Bug 1 regression: "result" key vs "result" value ────────────────────────

static void test_extract_string_key_vs_value() {
    // Exact shape of claude -p --output-format json output
    std::string json =
        R"({"type":"result","subtype":"success","result":"Hello world","total_cost_usd":0.03})";

    auto val = json::extract_string(json, "result");
    check(val.has_value(), "extract_string(result) returns a value");
    check(*val == "Hello world", "extract_string(result) == 'Hello world' (not 'success')");

    auto type_val = json::extract_string(json, "type");
    check(type_val.has_value(), "extract_string(type) returns a value");
    check(*type_val == "result", "extract_string(type) == 'result'");

    auto sub_val = json::extract_string(json, "subtype");
    check(sub_val.has_value(), "extract_string(subtype) returns a value");
    check(*sub_val == "success", "extract_string(subtype) == 'success'");
}

static void test_extract_string_with_whitespace() {
    std::string json = R"({ "type" : "result" , "result" : "actual text" })";

    auto val = json::extract_string(json, "result");
    check(val.has_value(), "extract_string with whitespace returns value");
    check(*val == "actual text", "extract_string with whitespace == 'actual text'");
}

static void test_extract_string_missing_key() {
    std::string json = R"({"type":"result","subtype":"success"})";
    auto val = json::extract_string(json, "nonexistent");
    check(!val.has_value(), "extract_string missing key returns nullopt");
}

static void test_extract_string_escapes() {
    std::string json = R"({"msg":"hello\nworld\t!"})";
    auto val = json::extract_string(json, "msg");
    check(val.has_value(), "extract_string with escapes returns value");
    check(*val == "hello\nworld\t!", "extract_string escapes decoded correctly");
}

// ─── Bug 1 regression: extract_number with value collision ───────────────────

static void test_extract_number_key_vs_value() {
    std::string json =
        R"({"type":"result","subtype":"success","result":"Hello world","total_cost_usd":0.03})";

    double cost = json::extract_number(json, "total_cost_usd");
    check(std::abs(cost - 0.03) < 0.001, "extract_number(total_cost_usd) == 0.03");
}

static void test_extract_number_fallback() {
    std::string json = R"({"name":"test"})";
    double val = json::extract_number(json, "missing_key", -1.0);
    check(std::abs(val - (-1.0)) < 0.001, "extract_number missing key returns fallback");
}

// ─── Bug 1 regression: extract_int with value collision ──────────────────────

static void test_extract_int_key_vs_value() {
    // "input_tokens" appears only as a key, but test key-verification anyway
    std::string json =
        R"({"type":"result","result":"text","usage":{"input_tokens":2,"output_tokens":18}})";

    int64_t in_tok = json::extract_int(json, "input_tokens");
    check(in_tok == 2, "extract_int(input_tokens) == 2");

    int64_t out_tok = json::extract_int(json, "output_tokens");
    check(out_tok == 18, "extract_int(output_tokens) == 18");
}

static void test_extract_int_fallback() {
    std::string json = R"({"x":10})";
    int64_t val = json::extract_int(json, "missing", -1);
    check(val == -1, "extract_int missing key returns fallback");
}

// ─── Bug 1 regression: extract_bool with value collision ─────────────────────

static void test_extract_bool_key_vs_value() {
    std::string json = R"({"is_error":false,"status":"false","verbose":true})";

    bool is_err = json::extract_bool(json, "is_error");
    check(is_err == false, "extract_bool(is_error) == false");

    bool verbose = json::extract_bool(json, "verbose");
    check(verbose == true, "extract_bool(verbose) == true");
}

static void test_extract_bool_fallback() {
    std::string json = R"({"x":true})";
    bool val = json::extract_bool(json, "missing", true);
    check(val == true, "extract_bool missing key returns fallback=true");
}

// ─── Full claude -p JSON regression ──────────────────────────────────────────

static void test_full_claude_json() {
    std::string json =
        R"({"type":"result","subtype":"success","is_error":false,)"
        R"("session_id":"abc123","result":"The answer is 42.",)"
        R"("total_cost_usd":0.029,"duration_ms":1500,)"
        R"("usage":{"input_tokens":2,"output_tokens":18}})";

    auto result = json::extract_string(json, "result");
    check(result.has_value(), "full: result has value");
    check(*result == "The answer is 42.", "full: result text correct");

    auto type = json::extract_string(json, "type");
    check(type.has_value() && *type == "result", "full: type == 'result'");

    auto subtype = json::extract_string(json, "subtype");
    check(subtype.has_value() && *subtype == "success", "full: subtype == 'success'");

    bool is_error = json::extract_bool(json, "is_error", true);
    check(is_error == false, "full: is_error == false");

    double cost = json::extract_number(json, "total_cost_usd");
    check(std::abs(cost - 0.029) < 0.0001, "full: total_cost_usd == 0.029");

    int64_t in_tok = json::extract_int(json, "input_tokens");
    check(in_tok == 2, "full: input_tokens == 2");

    int64_t out_tok = json::extract_int(json, "output_tokens");
    check(out_tok == 18, "full: output_tokens == 18");
}

// ─── Bug 2 regression: old key "cost_usd" should not match ──────────────────

static void test_old_cost_key_not_found() {
    std::string json =
        R"({"type":"result","result":"text","total_cost_usd":0.05})";

    double cost = json::extract_number(json, "cost_usd", -1.0);
    check(std::abs(cost - (-1.0)) < 0.001,
          "old key 'cost_usd' does not match 'total_cost_usd'");
}

// ─── Depth-0 extraction (Claude Code 2.1.261 envelope shape) ─────────────────
//
// The flat extractors return the FIRST "<key>": match at any depth. The real
// 2.1.261 result envelope nests usage.iterations[0]."type" == "message" ahead
// of the top-level "type":"result", which made the daemon reject every healthy
// reply. These tests pin the depth-0 path.

// A miniature of the real shape: the same key names appear inside a nested
// object AND inside an array element BEFORE the top-level ones.
static const std::string kNestedShadow =
    R"({"a":{"type":"nested-obj","result":"NESTED-OBJ"},)"
    R"("items":[{"type":"nested-arr","result":"NESTED-ARR","input_tokens":999,)"
    R"("nested_only":"HIDDEN"}],)"
    R"("type":"result","result":"TOPLEVEL","input_tokens":7,)"
    R"("is_error":false,"total_cost_usd":1.25,"api_error_status":null})";

static void test_flat_extractor_is_shadowed() {
    // Characterization of the bug being fixed: the flat scanner reads the
    // NESTED namesake. This is why extract_top_level_* exists.
    auto flat_type = json::extract_string(kNestedShadow, "type");
    check(flat_type.has_value() && *flat_type == "nested-obj",
          "flat extract_string(type) is shadowed by the nested key");
    int64_t flat_tok = json::extract_int(kNestedShadow, "input_tokens");
    check(flat_tok == 999, "flat extract_int(input_tokens) is shadowed by the array element");
}

static void test_top_level_ignores_nested() {
    auto type = json::extract_top_level_string(kNestedShadow, "type");
    check(type.has_value() && *type == "result",
          "top-level type == 'result' (not the nested 'nested-obj'/'nested-arr')");

    auto result = json::extract_top_level_string(kNestedShadow, "result");
    check(result.has_value() && *result == "TOPLEVEL",
          "top-level result == 'TOPLEVEL'");

    int64_t tok = json::extract_top_level_int(kNestedShadow, "input_tokens", -1);
    check(tok == 7, "top-level input_tokens == 7 (not the array element's 999)");

    bool is_err = json::extract_top_level_bool(kNestedShadow, "is_error", true);
    check(is_err == false, "top-level is_error == false");

    double cost = json::extract_top_level_number(kNestedShadow, "total_cost_usd", -1.0);
    check(std::abs(cost - 1.25) < 1e-9, "top-level total_cost_usd == 1.25");
}

static void test_top_level_missing_and_nested_only_keys() {
    auto missing = json::extract_top_level_string(kNestedShadow, "nonexistent");
    check(!missing.has_value(), "top-level missing key returns nullopt");

    // Present in the document, but only INSIDE an array element.
    auto nested_only = json::extract_top_level_string(kNestedShadow, "nested_only");
    check(!nested_only.has_value(), "key that exists only nested returns nullopt at depth 0");
    auto flat_nested_only = json::extract_string(kNestedShadow, "nested_only");
    check(flat_nested_only.has_value(), "…while the flat scanner does find it (contrast)");

    // A null value is not a string.
    auto null_val = json::extract_top_level_string(kNestedShadow, "api_error_status");
    check(!null_val.has_value(), "top-level null value returns nullopt for string");

    int64_t int_fb = json::extract_top_level_int(kNestedShadow, "nonexistent", -5);
    check(int_fb == -5, "top-level int missing key returns fallback");
    double num_fb = json::extract_top_level_number(kNestedShadow, "nonexistent", -2.5);
    check(std::abs(num_fb - (-2.5)) < 1e-9, "top-level number missing key returns fallback");
    bool bool_fb = json::extract_top_level_bool(kNestedShadow, "nonexistent", true);
    check(bool_fb == true, "top-level bool missing key returns fallback");
}

static void test_top_level_string_escapes_do_not_derail_scan() {
    // An earlier value contains an escaped quote, an escaped backslash and a
    // decoy "type": pair inside the string. A scanner that mishandles \" loses
    // sync and the whole object stops parsing.
    std::string json =
        R"({"note":"see \"type\": \"decoy\" here","tail":"trailing backslash \\",)"
        R"("type":"result","result":"real \"quoted\" text"})";

    auto type = json::extract_top_level_string(json, "type");
    check(type.has_value() && *type == "result",
          "escaped quotes in an earlier value do not derail the depth-0 scan");

    auto note = json::extract_top_level_string(json, "note");
    check(note.has_value() && *note == "see \"type\": \"decoy\" here",
          "escaped quotes decode correctly");

    auto tail = json::extract_top_level_string(json, "tail");
    check(tail.has_value() && *tail == "trailing backslash \\",
          "escaped backslash decodes correctly");

    auto result = json::extract_top_level_string(json, "result");
    check(result.has_value() && *result == "real \"quoted\" text",
          "top-level result with embedded quotes decodes correctly");
}

static void test_top_level_object_scopes_usage() {
    // Real 2.1.261 shape: usage.iterations[] repeats the token keys, and here
    // it precedes the totals, so a flat read reports one round-trip.
    std::string json =
        R"({"usage":{"iterations":[{"input_tokens":7,"output_tokens":1,"type":"message"}],)"
        R"("input_tokens":100,"output_tokens":200,"cache_creation_input_tokens":300,)"
        R"("cache_read_input_tokens":400},"type":"result"})";

    auto usage = json::extract_top_level_object(json, "usage");
    check(usage.has_value(), "extract_top_level_object(usage) returns the object");
    check(usage->front() == '{' && usage->back() == '}',
          "extracted usage object keeps its braces");

    check(json::extract_top_level_int(*usage, "input_tokens", -1) == 100,
          "usage.input_tokens == 100 (not iterations[0]'s 7)");
    check(json::extract_top_level_int(*usage, "output_tokens", -1) == 200,
          "usage.output_tokens == 200 (not iterations[0]'s 1)");
    check(json::extract_top_level_int(*usage, "cache_creation_input_tokens", -1) == 300,
          "usage.cache_creation_input_tokens == 300");
    check(json::extract_top_level_int(*usage, "cache_read_input_tokens", -1) == 400,
          "usage.cache_read_input_tokens == 400");

    // Contrast: the flat scan on the whole document takes the iteration.
    check(json::extract_int(json, "input_tokens") == 7,
          "flat extract_int on the whole document takes iterations[0] (contrast)");

    auto not_an_object = json::extract_top_level_object(json, "type");
    check(!not_an_object.has_value(), "extract_top_level_object on a string value returns nullopt");
}

static void test_top_level_tolerates_stderr_preamble_and_junk() {
    // claude -p runs with 2>&1, so warning lines can precede the envelope.
    std::string json =
        "Warning: something happened {not json}\n"
        R"({"type":"result","result":"OK"})" "\n";
    auto type = json::extract_top_level_string(json, "type");
    check(type.has_value() && *type == "result",
          "envelope is found after a stderr preamble");

    // No well-formed object at all.
    std::string plain = "Something went wrong but process exited 0";
    check(!json::extract_top_level_string(plain, "type").has_value(),
          "plain text returns nullopt");
    check(!json::extract_top_level_string("", "type").has_value(),
          "empty input returns nullopt");

    // Truncated envelope: not well-formed, so no value is invented.
    std::string truncated = R"({"type":"result","result":"half a rep)";
    check(!json::extract_top_level_string(truncated, "type").has_value(),
          "truncated envelope returns nullopt");
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== test_json (utils/json.h) ===\n\n";

    test_extract_string_key_vs_value();
    test_extract_string_with_whitespace();
    test_extract_string_missing_key();
    test_extract_string_escapes();
    test_extract_number_key_vs_value();
    test_extract_number_fallback();
    test_extract_int_key_vs_value();
    test_extract_int_fallback();
    test_extract_bool_key_vs_value();
    test_extract_bool_fallback();
    test_full_claude_json();
    test_old_cost_key_not_found();

    test_flat_extractor_is_shadowed();
    test_top_level_ignores_nested();
    test_top_level_missing_and_nested_only_keys();
    test_top_level_string_escapes_do_not_derail_scan();
    test_top_level_object_scopes_usage();
    test_top_level_tolerates_stderr_preamble_and_junk();

    std::cout << "\nAll tests passed.\n";
    return 0;
}
