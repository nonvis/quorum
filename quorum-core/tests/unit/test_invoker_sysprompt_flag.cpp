// tests/unit/test_invoker_sysprompt_flag.cpp
// Phase 7 Track 5 — Invoker::build_system_prompt_flag() pure helper.
//
// Mirrors the test_invoker_mode pattern: exercises the static flag-builder
// in isolation, no DB, no subprocess.
//
// Run:  cd build && ctest -R test_invoker_sysprompt_flag --output-on-failure

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

int main() {
    std::cout << "=====================================================\n";
    std::cout << "  Phase 7 Track 5 — build_system_prompt_flag tests\n";
    std::cout << "=====================================================\n";

    // F1: empty path → empty string. The invoker uses this as the no-op
    // path when system_prompt_body is empty (no CONTEXT.md / SKILL.md /
    // output rules to cache).
    {
        auto flags = Invoker::build_system_prompt_flag("");
        check(flags.empty(),
              "[F1] empty path -> empty flag string");
    }

    // F2: non-empty path → " --append-system-prompt-file <path>". The
    // leading space matches the build_tool_flags() concatenation contract
    // so the invoker can splice it directly into the cmd string.
    {
        auto flags = Invoker::build_system_prompt_flag(
            "/tmp/quorum_sysprompt_42.txt");
        check(flags == " --append-system-prompt-file /tmp/quorum_sysprompt_42.txt",
              "[F2] non-empty path -> ' --append-system-prompt-file <path>'");
    }

    // F3: path with spaces is NOT shell-escaped. Quoting is the caller's
    // responsibility — matching tool_flags conventions. In practice the
    // daemon only ever supplies /tmp/quorum_sysprompt_<task_id>.txt paths
    // (numeric task ids, no whitespace), so no escaping is needed.
    {
        auto flags = Invoker::build_system_prompt_flag("/tmp/with space/x.txt");
        check(flags == " --append-system-prompt-file /tmp/with space/x.txt",
              "[F3] spaces in path are passed through unescaped");
        // The space in the path appears literally, not as %20 or \ -escaped.
        check(flags.find("\\ ") == std::string::npos,
              "[F3] no backslash-space escape introduced");
        check(flags.find("%20") == std::string::npos,
              "[F3] no URL escape introduced");
    }

    return g_failed == 0 ? 0 : 1;
}
