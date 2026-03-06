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

    std::cout << "\nAll tests passed.\n";
    return 0;
}
