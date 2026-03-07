// tests/unit/test_invoker.cpp
// Unit tests for Invoker's CommandResult validation logic.
// Tests the static validate_claude_output() helper without needing a DB.
// Run:  cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure

#include <cassert>
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

// --- Test 3: Valid claude -p output --------------------------------------------

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

    std::cout << "\nAll tests passed.\n";
    return 0;
}
