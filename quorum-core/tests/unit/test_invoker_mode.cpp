// tests/unit/test_invoker_mode.cpp
// Unit tests for Invoker::build_tool_flags(): the pure function that
// decides what --allowedTools / --disallowedTools claude -p receives based
// on (agent_class, conversation_mode).
//
// Phase 6 Track 2: brainstorm mode forces a read-only tool surface for
// every agent in the conversation, OVERRIDING agent_class. Generic mode
// preserves pre-Phase-6 behavior (executor → full, others → read-only).
//
// These tests do NOT spawn `claude -p` — they exercise the pure function
// that produces the flag string.
//
// Run:  cd build && ctest -R test_invoker_mode --output-on-failure

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

#include "agent/invoker.h"

using sui::quorum::Invoker;

static int g_failures = 0;

static void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        ++g_failures;
    } else {
        std::cout << "[PASS] " << msg << "\n";
    }
}

static bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

// --- Case A: executor + generic → no tool restrictions ------------------------
//
// Pre-Phase-6 behavior: executor agents got full tool access (no --allowedTools,
// no --disallowedTools). The flag segment should be empty.
static void test_executor_generic_unchanged() {
    auto flags = Invoker::build_tool_flags("executor", "generic");
    check(flags.empty(),
          "[A] executor+generic: flag segment is empty (full tool access preserved)");
    check(!contains(flags, "--allowedTools"),
          "[A] executor+generic: no --allowedTools flag");
    check(!contains(flags, "--disallowedTools"),
          "[A] executor+generic: no --disallowedTools flag");
}

// --- Case B: executor + brainstorm → read-only sandbox (override) -------------
//
// The defining Phase 6 Track 2 invariant: a doer with agent_class: executor
// must NOT be able to mutate the project while the conversation is in
// brainstorm. The flag set must clamp to (Read, Grep, Glob).
static void test_executor_brainstorm_sandboxed() {
    auto flags = Invoker::build_tool_flags("executor", "brainstorm");
    check(contains(flags, "--allowedTools"),
          "[B] executor+brainstorm: contains --allowedTools");
    check(contains(flags, "Read") && contains(flags, "Grep") && contains(flags, "Glob"),
          "[B] executor+brainstorm: allowed = Read,Grep,Glob");
    check(contains(flags, "--disallowedTools"),
          "[B] executor+brainstorm: contains --disallowedTools");
    check(contains(flags, "Edit") && contains(flags, "Write") && contains(flags, "Bash"),
          "[B] executor+brainstorm: disallowed includes Edit,Write,Bash");
    check(contains(flags, "NotebookEdit"),
          "[B] executor+brainstorm: disallowed includes NotebookEdit");
}

// --- Case C: analyst + generic → existing analyst restriction ----------------
//
// Pre-Phase-6 behavior preserved: analyst-class agents are read-only via a
// disallow list (Write, Edit, NotebookEdit). No allowed-list constraint.
static void test_analyst_generic_unchanged() {
    auto flags = Invoker::build_tool_flags("analyst", "generic");
    check(contains(flags, "--disallowedTools"),
          "[C] analyst+generic: contains --disallowedTools");
    check(contains(flags, "Write") && contains(flags, "Edit") && contains(flags, "NotebookEdit"),
          "[C] analyst+generic: disallowed = Write,Edit,NotebookEdit");
    check(!contains(flags, "--allowedTools"),
          "[C] analyst+generic: no --allowedTools flag (only disallow list)");
}

// --- Case D: analyst + brainstorm → brainstorm read-only set -----------------
//
// No behavioral difference vs analyst-in-generic for the *practical* tool set
// (both are read-only), but assert it explicitly: brainstorm always emits the
// allowed-list form, not the disallow-only form. This makes the override
// path observable from logs.
static void test_analyst_brainstorm_sandboxed() {
    auto flags = Invoker::build_tool_flags("analyst", "brainstorm");
    check(contains(flags, "--allowedTools"),
          "[D] analyst+brainstorm: contains --allowedTools (allow-list form)");
    check(contains(flags, "Read") && contains(flags, "Grep") && contains(flags, "Glob"),
          "[D] analyst+brainstorm: allowed = Read,Grep,Glob");
    check(contains(flags, "--disallowedTools"),
          "[D] analyst+brainstorm: contains --disallowedTools");
    check(contains(flags, "Edit") && contains(flags, "Write") && contains(flags, "Bash"),
          "[D] analyst+brainstorm: disallowed includes Edit,Write,Bash");
}

// --- Case E: unknown mode → falls back to generic behavior --------------------
//
// Mirrors Cycle 1 pattern (DB / config layers): unrecognized mode strings
// silently fall back to "generic" semantics rather than crash. Equivalent to
// Case A for executors, Case C for analysts.
static void test_unknown_mode_falls_back_to_generic() {
    auto flags_exec = Invoker::build_tool_flags("executor", "xyz");
    check(flags_exec.empty(),
          "[E] executor+'xyz': empty flags (fell back to generic-executor)");

    auto flags_analyst = Invoker::build_tool_flags("analyst", "xyz");
    check(contains(flags_analyst, "--disallowedTools"),
          "[E] analyst+'xyz': falls back to analyst-generic disallow list");
    check(!contains(flags_analyst, "--allowedTools"),
          "[E] analyst+'xyz': no --allowedTools (not brainstorm)");

    // Empty mode string also treated as not-brainstorm.
    auto flags_empty = Invoker::build_tool_flags("executor", "");
    check(flags_empty.empty(),
          "[E] executor+'': empty mode treated as generic");
}

// --- main ---------------------------------------------------------------------

int main() {
    std::cout << "=== test_invoker_mode (Phase 6 Track 2 sandboxing) ===\n\n";

    test_executor_generic_unchanged();
    test_executor_brainstorm_sandboxed();
    test_analyst_generic_unchanged();
    test_analyst_brainstorm_sandboxed();
    test_unknown_mode_falls_back_to_generic();

    if (g_failures > 0) {
        std::cerr << "\n" << g_failures << " test(s) FAILED.\n";
        return 1;
    }
    std::cout << "\nAll tests passed.\n";
    return 0;
}
