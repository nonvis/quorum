// tests/unit/test_output_parser.cpp
// Inline test for OutputParser — compile via CMake target test_output_parser.
// Run:  cd build && ctest --output-on-failure  (or ./test_output_parser)

#include <cassert>
#include <iostream>
#include <string>

#include "agent/output_parser.h"

// ─── helpers ─────────────────────────────────────────────────────────────────

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        std::exit(1);
    }
    std::cout << "[PASS] " << msg << "\n";
}

// ─── sample input ─────────────────────────────────────────────────────────────

static const std::string SAMPLE = R"(Here is my analysis of market conditions.

```VAULT_UPDATE
path: knowledge/market-analysis.md
content: |
  # Market Analysis
  The market is showing bullish trends.
  Key indicators are positive.
```

After deep review, I propose the following action.

```PROPOSAL
title: Increase Position Size
requires_consensus_from: [risk_analyst, operator]
content: |
  Given the positive market conditions,
  I recommend increasing our position size by 20%.
```

```REVIEW
proposal_id: proposal-042
verdict: approve
reasoning: |
  The analysis is sound.
  Risk levels are acceptable.
```

```SUMMARY
Market conditions are positive. Recommending position increase pending consensus.
```

Some trailing free text.)";

// ─── tests ───────────────────────────────────────────────────────────────────

static void test_all_four_blocks() {
    sui::quorum::OutputParser parser;
    auto r = parser.parse(SAMPLE);

    std::cout << "\n=== Parsed Output ===\n";
    std::cout << "summary       : " << r.summary << "\n";
    std::cout << "vault_updates : " << r.vault_updates.size() << "\n";
    std::cout << "proposals     : " << r.proposals.size() << "\n";
    std::cout << "reviews       : " << r.reviews.size() << "\n";
    std::cout << "free_text     :\n" << r.free_text << "\n";
    std::cout << "has_actionable: " << (parser.has_actionable_output(r) ? "true" : "false") << "\n\n";

    // SUMMARY
    check(r.summary == "Market conditions are positive. Recommending position increase pending consensus.",
          "summary text matches");

    // VAULT_UPDATE
    check(r.vault_updates.size() == 1, "one vault update");
    check(r.vault_updates[0].path == "knowledge/market-analysis.md", "vault path");
    check(r.vault_updates[0].content.find("bullish") != std::string::npos, "vault content contains 'bullish'");
    check(r.vault_updates[0].content.find("Market Analysis") != std::string::npos, "vault content contains header");

    // PROPOSAL
    check(r.proposals.size() == 1, "one proposal");
    check(r.proposals[0].title == "Increase Position Size", "proposal title");
    check(r.proposals[0].requires_consensus_from.size() == 2, "two reviewers");
    check(r.proposals[0].requires_consensus_from[0] == "risk_analyst", "reviewer[0] = risk_analyst");
    check(r.proposals[0].requires_consensus_from[1] == "operator", "reviewer[1] = operator");
    check(r.proposals[0].content.find("20%") != std::string::npos, "proposal content contains '20%'");

    // REVIEW
    check(r.reviews.size() == 1, "one review");
    check(r.reviews[0].proposal_id == "proposal-042", "review proposal_id");
    check(r.reviews[0].verdict == "approve", "review verdict");
    check(r.reviews[0].reasoning.find("sound") != std::string::npos, "reasoning contains 'sound'");

    // free_text must contain the prose outside blocks
    check(r.free_text.find("Here is my analysis") != std::string::npos, "free_text contains opening prose");
    check(r.free_text.find("trailing free text") != std::string::npos, "free_text contains trailing prose");

    // has_actionable_output
    check(parser.has_actionable_output(r), "has_actionable_output is true");
}

static void test_empty_input() {
    sui::quorum::OutputParser parser;
    auto r = parser.parse("");
    check(r.summary.empty(), "empty: summary is empty");
    check(r.vault_updates.empty(), "empty: no vault updates");
    check(r.proposals.empty(), "empty: no proposals");
    check(r.reviews.empty(), "empty: no reviews");
    check(r.free_text.empty(), "empty: free_text is empty");
    check(!parser.has_actionable_output(r), "empty: has_actionable_output is false");
}

static void test_free_text_only() {
    sui::quorum::OutputParser parser;
    auto r = parser.parse("Just some plain text.\nNo blocks here.");
    check(r.free_text == "Just some plain text.\nNo blocks here.", "free_text only");
    check(r.summary.empty(), "no summary");
    check(!parser.has_actionable_output(r), "no actionable output");
}

static void test_multiple_vault_updates() {
    sui::quorum::OutputParser parser;
    std::string input =
        "```VAULT_UPDATE\npath: a.md\ncontent: |\n  content-a\n```\n"
        "```VAULT_UPDATE\npath: b.md\ncontent: |\n  content-b\n```\n";
    auto r = parser.parse(input);
    check(r.vault_updates.size() == 2, "two vault updates");
    check(r.vault_updates[0].path == "a.md", "first path = a.md");
    check(r.vault_updates[1].path == "b.md", "second path = b.md");
    check(r.vault_updates[0].content == "content-a", "first content");
    check(r.vault_updates[1].content == "content-b", "second content");
}

static void test_summary_only() {
    sui::quorum::OutputParser parser;
    std::string input = "```SUMMARY\nShort summary line.\n```\n";
    auto r = parser.parse(input);
    check(r.summary == "Short summary line.", "summary only");
    check(!parser.has_actionable_output(r), "summary alone is not actionable");
}

static void test_list_single_item() {
    sui::quorum::OutputParser parser;
    std::string input =
        "```PROPOSAL\ntitle: Solo\nrequires_consensus_from: [agent_x]\ncontent: |\n  details\n```\n";
    auto r = parser.parse(input);
    check(r.proposals.size() == 1, "one proposal (single reviewer)");
    check(r.proposals[0].requires_consensus_from.size() == 1, "one reviewer");
    check(r.proposals[0].requires_consensus_from[0] == "agent_x", "reviewer = agent_x");
}

static void test_single_observation() {
    sui::quorum::OutputParser parser;
    std::string input =
        "```OBSERVATION\n"
        "title: Session 30 Adverse Selection Spike\n"
        "tags: [mm-bot, adverse-selection, session-30]\n"
        "content: |\n"
        "  15 adverse fills totaling -4.8 SUI.\n"
        "  Spread of 30bps insufficient during high-vol windows.\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.observations.size() == 1, "obs: one observation");
    check(r.observations[0].title == "Session 30 Adverse Selection Spike", "obs: title matches");
    check(r.observations[0].tags.size() == 3, "obs: tags has 3 items");
    check(r.observations[0].tags[0] == "mm-bot", "obs: tag[0] = mm-bot");
    check(r.observations[0].tags[1] == "adverse-selection", "obs: tag[1] = adverse-selection");
    check(r.observations[0].tags[2] == "session-30", "obs: tag[2] = session-30");
    check(r.observations[0].content.find("adverse fills") != std::string::npos, "obs: content contains 'adverse fills'");
    check(r.observations[0].agent.empty(), "obs: agent is empty (not parsed)");
    check(r.observations[0].task_type.empty(), "obs: task_type is empty (not parsed)");
    check(parser.has_actionable_output(r), "obs: has_actionable_output is true");
}

static void test_mixed_with_observation() {
    sui::quorum::OutputParser parser;
    std::string input =
        "```VAULT_UPDATE\n"
        "path: knowledge/notes.md\n"
        "content: |\n"
        "  Some notes here.\n"
        "```\n"
        "```OBSERVATION\n"
        "title: Spread Anomaly\n"
        "tags: [spread, anomaly]\n"
        "content: |\n"
        "  Observed unusual spread widening.\n"
        "```\n"
        "```PROPOSAL\n"
        "title: Adjust Parameters\n"
        "requires_consensus_from: [operator]\n"
        "content: |\n"
        "  Recommend parameter adjustment.\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.vault_updates.size() == 1, "mixed: one vault update");
    check(r.observations.size() == 1, "mixed: one observation");
    check(r.proposals.size() == 1, "mixed: one proposal");
    check(r.vault_updates[0].path == "knowledge/notes.md", "mixed: vault path");
    check(r.observations[0].title == "Spread Anomaly", "mixed: observation title");
    check(r.observations[0].tags.size() == 2, "mixed: observation tags count");
    check(r.proposals[0].title == "Adjust Parameters", "mixed: proposal title");
}

static void test_observation_no_tags() {
    sui::quorum::OutputParser parser;
    std::string input =
        "```OBSERVATION\n"
        "title: Simple Observation\n"
        "content: |\n"
        "  Just a note without tags.\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.observations.size() == 1, "no-tags: one observation");
    check(r.observations[0].tags.empty(), "no-tags: tags vector is empty");
    check(r.observations[0].title == "Simple Observation", "no-tags: title parsed");
    check(r.observations[0].content.find("without tags") != std::string::npos, "no-tags: content parsed");
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== test_output_parser ===\n\n";

    test_all_four_blocks();
    test_empty_input();
    test_free_text_only();
    test_multiple_vault_updates();
    test_summary_only();
    test_list_single_item();
    test_single_observation();
    test_mixed_with_observation();
    test_observation_no_tags();

    std::cout << "\nAll tests passed.\n";
    return 0;
}
