// tests/unit/test_handoff_parser.cpp
// Unit tests for HANDOFF block parsing in OutputParser.
//
// Run:  cd build && cmake .. && make test_handoff_parser && ./test_handoff_parser

#include <cstdlib>
#include <iostream>
#include <string>

#include "agent/output_parser.h"

static int g_passed = 0;
static int g_failed = 0;

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        ++g_failed;
        std::exit(1);
    }
    std::cout << "[PASS] " << msg << "\n";
    ++g_passed;
}

// A. Basic HANDOFF block
static void test_basic_handoff() {
    std::cout << "\n=== A. Basic HANDOFF ===\n\n";
    sui::quorum::OutputParser parser;
    auto r = parser.parse(R"(
```HANDOFF
to: move-dev
prompt: implement the storage module
```
)");
    check(r.handoff.has_value(), "A: handoff present");
    check(r.handoff->to == "move-dev", "A: to == move-dev");
    check(r.handoff->prompt == "implement the storage module", "A: prompt matches");
}

// B. Multi-line prompt (pipe syntax)
static void test_multiline_prompt() {
    std::cout << "\n=== B. Multi-line Prompt ===\n\n";
    sui::quorum::OutputParser parser;
    auto r = parser.parse(R"(
```HANDOFF
to: thinker
prompt: |
  The doer found a problem with the approach.
  Please revise the plan to handle edge cases.
```
)");
    check(r.handoff.has_value(), "B: handoff present");
    check(r.handoff->to == "thinker", "B: to == thinker");
    check(r.handoff->prompt.find("doer found a problem") != std::string::npos,
          "B: prompt contains first line");
    check(r.handoff->prompt.find("revise the plan") != std::string::npos,
          "B: prompt contains second line");
}

// C. Special to values (human, done)
static void test_special_to_values() {
    std::cout << "\n=== C. Special to Values ===\n\n";
    sui::quorum::OutputParser parser;

    auto r1 = parser.parse(R"(
```HANDOFF
to: human
prompt: need clarification on the API design
```
)");
    check(r1.handoff.has_value(), "C1: handoff present");
    check(r1.handoff->to == "human", "C1: to == human");

    auto r2 = parser.parse(R"(
```HANDOFF
to: done
```
)");
    check(r2.handoff.has_value(), "C2: handoff present");
    check(r2.handoff->to == "done", "C2: to == done");
}

// D. Missing to field — malformed, ignored
static void test_missing_to() {
    std::cout << "\n=== D. Missing to Field ===\n\n";
    sui::quorum::OutputParser parser;
    auto r = parser.parse(R"(
```HANDOFF
prompt: this has no destination
```
)");
    check(!r.handoff.has_value(), "D: handoff is nullopt (missing to)");
}

// E. Missing prompt — valid, prompt is empty
static void test_missing_prompt() {
    std::cout << "\n=== E. Missing Prompt ===\n\n";
    sui::quorum::OutputParser parser;
    auto r = parser.parse(R"(
```HANDOFF
to: done
```
)");
    check(r.handoff.has_value(), "E: handoff present");
    check(r.handoff->to == "done", "E: to == done");
    check(r.handoff->prompt.empty(), "E: prompt is empty");
}

// F. HANDOFF coexists with SUMMARY
static void test_handoff_with_summary() {
    std::cout << "\n=== F. HANDOFF with SUMMARY ===\n\n";
    sui::quorum::OutputParser parser;
    auto r = parser.parse(R"(
```SUMMARY
Built the module, all tests pass.
```

```HANDOFF
to: scribe
prompt: record the architecture decisions from this cycle
```
)");
    check(!r.summary.empty(), "F: summary populated");
    check(r.summary.find("Built the module") != std::string::npos, "F: summary content");
    check(r.handoff.has_value(), "F: handoff present");
    check(r.handoff->to == "scribe", "F: to == scribe");
    check(r.handoff->prompt.find("architecture decisions") != std::string::npos,
          "F: prompt content");
}

// G. Multiple HANDOFF blocks — last one wins
static void test_multiple_handoffs() {
    std::cout << "\n=== G. Multiple HANDOFFs ===\n\n";
    sui::quorum::OutputParser parser;
    auto r = parser.parse(R"(
```HANDOFF
to: thinker
prompt: first attempt
```

```HANDOFF
to: reviewer
prompt: second attempt
```
)");
    check(r.handoff.has_value(), "G: handoff present");
    check(r.handoff->to == "reviewer", "G: to == reviewer (last wins)");
    check(r.handoff->prompt == "second attempt", "G: prompt == second attempt");
}

// H. Heading format (## HANDOFF above plain fence)
static void test_heading_format() {
    std::cout << "\n=== H. Heading Format ===\n\n";
    sui::quorum::OutputParser parser;
    auto r = parser.parse(R"(
## HANDOFF
```
to: move-dev
prompt: implement it
```
)");
    check(r.handoff.has_value(), "H: handoff present");
    check(r.handoff->to == "move-dev", "H: to == move-dev");
    check(r.handoff->prompt == "implement it", "H: prompt matches");
}

// I. Content fallback — no prompt: field, free text after to:
static void test_content_fallback() {
    std::cout << "\n=== I. Content Fallback ===\n\n";
    sui::quorum::OutputParser parser;
    auto r = parser.parse(R"(
```HANDOFF
to: leader
I'm stuck and need help deciding the approach.
The API has two possible patterns.
```
)");
    check(r.handoff.has_value(), "I: handoff present");
    check(r.handoff->to == "leader", "I: to == leader");
    check(r.handoff->prompt.find("stuck and need help") != std::string::npos,
          "I: prompt contains first line");
    check(r.handoff->prompt.find("two possible patterns") != std::string::npos,
          "I: prompt contains second line");
}

int main() {
    std::cout << "=== HANDOFF Parser Tests ===\n";

    test_basic_handoff();
    test_multiline_prompt();
    test_special_to_values();
    test_missing_to();
    test_missing_prompt();
    test_handoff_with_summary();
    test_multiple_handoffs();
    test_heading_format();
    test_content_fallback();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";
    return g_failed > 0 ? 1 : 0;
}
