// tests/unit/test_evaluation_parser.cpp
// Unit tests for EVALUATION block parsing in OutputParser (Phase 8 Track 3).
//
// Run:  cd build && cmake .. && make test_evaluation_parser && ./test_evaluation_parser

#include <cmath>
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

// A. Well-formed EVALUATION block — all fields parsed correctly.
static void test_A_well_formed() {
    std::cout << "\n=== A. Well-formed EVALUATION ===\n\n";
    sui::quorum::OutputParser parser;
    auto r = parser.parse(R"(
```EVALUATION
role: move-dev
rubric_version: v1
total: 78
items_json: [{"id":"compile-clean","weight":5,"passed":true},{"id":"move-2024","weight":4,"passed":false,"notes":"old syntax"}]
notes: Solid implementation but missed several modernizations.
```
)");
    check(r.evaluation.has_value(), "A: evaluation present");
    check(r.evaluation->role_specialty == "move-dev", "A: role_specialty == move-dev");
    check(r.evaluation->rubric_version == "v1", "A: rubric_version == v1");
    check(std::fabs(r.evaluation->total_score - 78.0) < 1e-9, "A: total_score == 78.0");
    check(r.evaluation->items_json.find("compile-clean") != std::string::npos,
          "A: items_json preserves 'compile-clean'");
    check(r.evaluation->items_json.find("move-2024") != std::string::npos,
          "A: items_json preserves 'move-2024'");
    check(r.evaluation->notes.find("Solid implementation") != std::string::npos,
          "A: notes preserved");
    check(r.evaluation->scored.empty(), "A: scored field empty (not provided)");
}

// B. Malformed items_json — parser still returns block, score_json contains
//    raw string. Must not crash.
static void test_B_malformed_items_json() {
    std::cout << "\n=== B. Malformed items_json ===\n\n";
    sui::quorum::OutputParser parser;
    auto r = parser.parse(R"(
```EVALUATION
role: move-dev
rubric_version: v1
total: 50
items_json: not actually json {{ broken
notes: Score with malformed items.
```
)");
    check(r.evaluation.has_value(), "B: evaluation present despite malformed json");
    check(std::fabs(r.evaluation->total_score - 50.0) < 1e-9, "B: total_score == 50.0");
    check(r.evaluation->items_json.find("not actually json") != std::string::npos,
          "B: raw items_json string preserved verbatim");
    check(r.evaluation->notes.find("malformed") != std::string::npos,
          "B: notes preserved");
}

// C. Missing required fields — parser returns nullopt for that block.
static void test_C_missing_required_fields() {
    std::cout << "\n=== C. Missing Required Fields ===\n\n";
    sui::quorum::OutputParser parser;

    // C1. Missing total
    auto r1 = parser.parse(R"(
```EVALUATION
role: move-dev
rubric_version: v1
notes: No total provided.
```
)");
    check(!r1.evaluation.has_value(), "C1: missing total -> nullopt");

    // C2. Missing role
    auto r2 = parser.parse(R"(
```EVALUATION
rubric_version: v1
total: 80
notes: No role.
```
)");
    check(!r2.evaluation.has_value(), "C2: missing role -> nullopt");

    // C3. Missing rubric_version
    auto r3 = parser.parse(R"(
```EVALUATION
role: move-dev
total: 80
notes: No version.
```
)");
    check(!r3.evaluation.has_value(), "C3: missing rubric_version -> nullopt");
}

// D. EVALUATION coexists with HANDOFF and SUMMARY in the same output.
//    All three must be parsed correctly.
static void test_D_coexists_with_handoff_summary() {
    std::cout << "\n=== D. Coexists with HANDOFF + SUMMARY ===\n\n";
    sui::quorum::OutputParser parser;
    auto r = parser.parse(R"(
Some preamble from the agent.

```SUMMARY
Scored move-dev work: 78/100, missed some 2024 idioms.
```

```EVALUATION
role: move-dev
rubric_version: v1
total: 78
items_json: [{"id":"compile","weight":5,"passed":true}]
notes: Solid but unidiomatic.
scored: doer
```

```HANDOFF
to: scribe
prompt: Task 1: record evaluation
```
)");
    check(r.evaluation.has_value(), "D: evaluation present");
    check(r.evaluation->role_specialty == "move-dev", "D: eval role_specialty");
    check(std::fabs(r.evaluation->total_score - 78.0) < 1e-9, "D: eval score");
    check(r.evaluation->scored == "doer", "D: scored == doer");

    check(r.summary.find("Scored move-dev") != std::string::npos,
          "D: SUMMARY content captured");

    check(r.handoff.has_value(), "D: handoff present");
    check(r.handoff->to == "scribe", "D: handoff.to == scribe");
    check(r.handoff->prompt.find("Task 1") != std::string::npos,
          "D: handoff prompt preserved");
}

// E. Numeric total — accept "78" / "78.5" / "78.0", reject "78%".
static void test_E_numeric_total_forms() {
    std::cout << "\n=== E. Numeric total forms ===\n\n";
    sui::quorum::OutputParser parser;

    auto r1 = parser.parse(R"(
```EVALUATION
role: move-dev
rubric_version: v1
total: 78
items_json: []
notes: integer total.
```
)");
    check(r1.evaluation.has_value(), "E1: integer total accepted");
    check(std::fabs(r1.evaluation->total_score - 78.0) < 1e-9, "E1: total == 78.0");

    auto r2 = parser.parse(R"(
```EVALUATION
role: move-dev
rubric_version: v1
total: 78.5
items_json: []
notes: decimal total.
```
)");
    check(r2.evaluation.has_value(), "E2: decimal total accepted");
    check(std::fabs(r2.evaluation->total_score - 78.5) < 1e-9, "E2: total == 78.5");

    // % form — must be rejected (block dropped, stderr warning emitted).
    auto r3 = parser.parse(R"(
```EVALUATION
role: move-dev
rubric_version: v1
total: 78%
items_json: []
notes: percent suffix should reject.
```
)");
    check(!r3.evaluation.has_value(), "E3: 78% form rejected");

    // Junk text — must be rejected.
    auto r4 = parser.parse(R"(
```EVALUATION
role: move-dev
rubric_version: v1
total: not-a-number
items_json: []
notes: garbage total.
```
)");
    check(!r4.evaluation.has_value(), "E4: non-numeric total rejected");
}

// F. Empty items_json field is allowed (the LLM might emit no per-item data).
static void test_F_empty_items_json() {
    std::cout << "\n=== F. Empty items_json ===\n\n";
    sui::quorum::OutputParser parser;
    auto r = parser.parse(R"(
```EVALUATION
role: move-dev
rubric_version: v1
total: 0
notes: no rubric found.
```
)");
    check(r.evaluation.has_value(), "F: eval present even with no items_json field");
    check(std::fabs(r.evaluation->total_score - 0.0) < 1e-9, "F: total == 0");
    check(r.evaluation->items_json.empty(), "F: items_json empty string");
}

int main() {
    std::cout << "=== EVALUATION Block Parser Tests ===\n";

    test_A_well_formed();
    test_B_malformed_items_json();
    test_C_missing_required_fields();
    test_D_coexists_with_handoff_summary();
    test_E_numeric_total_forms();
    test_F_empty_items_json();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";
    return g_failed > 0 ? 1 : 0;
}
