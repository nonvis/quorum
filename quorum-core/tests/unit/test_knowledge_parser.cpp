// tests/unit/test_knowledge_parser.cpp
// Unit tests for KNOWLEDGE block parsing in OutputParser.
//
// Run:  cd build && cmake .. && make test_knowledge_parser && ./test_knowledge_parser

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

// A. Basic KNOWLEDGE block
static void test_basic_knowledge() {
    std::cout << "\n=== A. Basic KNOWLEDGE ===\n\n";
    sui::quorum::OutputParser parser;
    auto r = parser.parse(R"(
```KNOWLEDGE
topic: architecture-decision
content: Decided to use WAL mode for SQLite.
```
)");
    check(r.knowledge.size() == 1, "A: knowledge.size() == 1");
    check(r.knowledge[0].topic == "architecture-decision", "A: topic matches");
    check(r.knowledge[0].content == "Decided to use WAL mode for SQLite.",
          "A: content matches");
}

// B. Multi-line content (pipe syntax)
static void test_multiline_content() {
    std::cout << "\n=== B. Multi-line Content ===\n\n";
    sui::quorum::OutputParser parser;
    auto r = parser.parse(R"(
```KNOWLEDGE
topic: api-change
content: |
  Added new endpoint /api/v2/users.
  Deprecated /api/v1/users.
```
)");
    check(r.knowledge.size() == 1, "B: knowledge.size() == 1");
    check(r.knowledge[0].content.find("Added new endpoint /api/v2/users.") != std::string::npos,
          "B: content contains first line");
    check(r.knowledge[0].content.find("Deprecated /api/v1/users.") != std::string::npos,
          "B: content contains second line");
}

// C. Multiple KNOWLEDGE blocks (all collected, not last-wins)
static void test_multiple_knowledge() {
    std::cout << "\n=== C. Multiple KNOWLEDGE Blocks ===\n\n";
    sui::quorum::OutputParser parser;
    auto r = parser.parse(R"(
```KNOWLEDGE
topic: decision
content: Chose approach A over B.
```

```KNOWLEDGE
topic: finding
content: Performance improved by 30%.
```
)");
    check(r.knowledge.size() == 2, "C: knowledge.size() == 2");
    check(r.knowledge[0].topic == "decision", "C: first topic == decision");
    check(r.knowledge[0].content == "Chose approach A over B.", "C: first content matches");
    check(r.knowledge[1].topic == "finding", "C: second topic == finding");
    check(r.knowledge[1].content == "Performance improved by 30%.", "C: second content matches");
}

// D. Missing content (block dropped)
static void test_missing_content() {
    std::cout << "\n=== D. Missing Content ===\n\n";
    sui::quorum::OutputParser parser;
    auto r = parser.parse(R"(
```KNOWLEDGE
topic: empty-observation
```
)");
    check(r.knowledge.empty(), "D: knowledge.empty() (no content)");
}

// E. Missing topic (valid, topic is empty)
static void test_missing_topic() {
    std::cout << "\n=== E. Missing Topic ===\n\n";
    sui::quorum::OutputParser parser;
    auto r = parser.parse(R"(
```KNOWLEDGE
content: Topicless observation.
```
)");
    check(r.knowledge.size() == 1, "E: knowledge.size() == 1");
    check(r.knowledge[0].topic.empty(), "E: topic is empty");
    check(r.knowledge[0].content == "Topicless observation.", "E: content populated");
}

// F. Content fallback (no content: field, free text after topic:)
static void test_content_fallback() {
    std::cout << "\n=== F. Content Fallback ===\n\n";
    sui::quorum::OutputParser parser;
    auto r = parser.parse(R"(
```KNOWLEDGE
topic: insight
The system handles 10K connections.
Memory stays at 2GB.
```
)");
    check(r.knowledge.size() == 1, "F: knowledge.size() == 1");
    check(r.knowledge[0].topic == "insight", "F: topic == insight");
    check(r.knowledge[0].content.find("The system handles 10K connections.") != std::string::npos,
          "F: content contains first line");
    check(r.knowledge[0].content.find("Memory stays at 2GB.") != std::string::npos,
          "F: content contains second line");
}

// G. KNOWLEDGE coexists with HANDOFF and SUMMARY
static void test_coexists_with_others() {
    std::cout << "\n=== G. Coexists with HANDOFF and SUMMARY ===\n\n";
    sui::quorum::OutputParser parser;
    auto r = parser.parse(R"(
```KNOWLEDGE
topic: arch
content: Decided on event-driven architecture.
```

```SUMMARY
Built the module successfully.
```

```HANDOFF
to: reviewer
prompt: please review the changes
```
)");
    check(r.knowledge.size() == 1, "G: knowledge.size() == 1");
    check(r.knowledge[0].topic == "arch", "G: knowledge topic");
    check(!r.summary.empty(), "G: summary populated");
    check(r.summary.find("Built the module") != std::string::npos, "G: summary content");
    check(r.handoff.has_value(), "G: handoff present");
    check(r.handoff->to == "reviewer", "G: handoff to == reviewer");
}

// H. Heading format (## KNOWLEDGE above plain fence)
static void test_heading_format() {
    std::cout << "\n=== H. Heading Format ===\n\n";
    sui::quorum::OutputParser parser;
    auto r = parser.parse(R"(
## KNOWLEDGE
```
topic: heading-style
content: Works with heading detection.
```
)");
    check(r.knowledge.size() == 1, "H: knowledge.size() == 1");
    check(r.knowledge[0].topic == "heading-style", "H: topic matches");
    check(r.knowledge[0].content == "Works with heading detection.", "H: content matches");
}

int main() {
    std::cout << "=== KNOWLEDGE Parser Tests ===\n";

    test_basic_knowledge();
    test_multiline_content();
    test_multiple_knowledge();
    test_missing_content();
    test_missing_topic();
    test_content_fallback();
    test_coexists_with_others();
    test_heading_format();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";
    return g_failed > 0 ? 1 : 0;
}
