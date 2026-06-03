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

// ─── lenient block detection tests ──────────────────────────────────────────

static void test_heading_vault_update() {
    sui::quorum::OutputParser parser;
    std::string input =
        "## VAULT_UPDATE\n"
        "```\n"
        "path: knowledge/foo.md\n"
        "content: |\n"
        "  some data here\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.vault_updates.size() == 1, "heading-vu: one vault update");
    check(r.vault_updates[0].path == "knowledge/foo.md", "heading-vu: path matches");
    check(r.vault_updates[0].content.find("some data") != std::string::npos,
          "heading-vu: content parsed");
}

static void test_bold_proposal() {
    sui::quorum::OutputParser parser;
    std::string input =
        "**PROPOSAL**\n"
        "```\n"
        "title: My Idea\n"
        "requires_consensus_from: [bot_analyst]\n"
        "content: |\n"
        "  details\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.proposals.size() == 1, "bold-prop: one proposal");
    check(r.proposals[0].title == "My Idea", "bold-prop: title matches");
    check(r.proposals[0].requires_consensus_from.size() == 1, "bold-prop: one reviewer");
    check(r.proposals[0].requires_consensus_from[0] == "bot_analyst",
          "bold-prop: reviewer matches");
}

static void test_first_line_type_fallback() {
    sui::quorum::OutputParser parser;
    std::string input =
        "```\n"
        "OBSERVATION\n"
        "title: Something\n"
        "tags: [x]\n"
        "content: |\n"
        "  data\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.observations.size() == 1, "first-line: one observation");
    check(r.observations[0].title == "Something", "first-line: title matches");
    check(r.observations[0].tags.size() == 1, "first-line: one tag");
}

static void test_bold_with_type_inside_dedup() {
    sui::quorum::OutputParser parser;
    std::string input =
        "**OBSERVATION**\n"
        "```\n"
        "OBSERVATION\n"
        "agent: market_analyst\n"
        "title: Pool Scan\n"
        "content: |\n"
        "  details\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.observations.size() == 1, "dedup: one observation");
    check(r.observations[0].title == "Pool Scan", "dedup: title is Pool Scan");
    check(r.observations[0].content.find("OBSERVATION") == std::string::npos,
          "dedup: OBSERVATION not in content");
}

static void test_heading_with_suffix() {
    sui::quorum::OutputParser parser;
    std::string input =
        "## VAULT_UPDATE \xe2\x80\x94 knowledge/latest.md\n"
        "```\n"
        "path: knowledge/latest.md\n"
        "content: |\n"
        "  analysis data\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.vault_updates.size() == 1, "heading-suffix: one vault update");
    check(r.vault_updates[0].path == "knowledge/latest.md",
          "heading-suffix: path matches");
}

static void test_bold_with_colon_suffix() {
    sui::quorum::OutputParser parser;
    std::string input =
        "**VAULT_UPDATE: knowledge/market.md**\n"
        "```\n"
        "path: knowledge/market.md\n"
        "content: |\n"
        "  market data\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.vault_updates.size() == 1, "bold-colon: one vault update");
    check(r.vault_updates[0].path == "knowledge/market.md",
          "bold-colon: path matches");
}

static void test_backward_compat_explicit_fence() {
    sui::quorum::OutputParser parser;
    std::string input =
        "```VAULT_UPDATE\n"
        "path: knowledge/compat.md\n"
        "content: |\n"
        "  old-style block\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.vault_updates.size() == 1, "compat: one vault update");
    check(r.vault_updates[0].path == "knowledge/compat.md", "compat: path matches");
    check(r.vault_updates[0].content.find("old-style") != std::string::npos,
          "compat: content parsed");
}

static void test_plain_fence_no_type_ignored() {
    sui::quorum::OutputParser parser;
    std::string input =
        "```\n"
        "hello world\n"
        "some code\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.vault_updates.empty(), "no-type: no vault updates");
    check(r.proposals.empty(), "no-type: no proposals");
    check(r.observations.empty(), "no-type: no observations");
    check(r.reviews.empty(), "no-type: no reviews");
}

static void test_no_type_leak_across_blocks() {
    sui::quorum::OutputParser parser;
    std::string input =
        "**PROPOSAL**\n"
        "```\n"
        "title: First\n"
        "requires_consensus_from: [x]\n"
        "content: |\n"
        "  a\n"
        "```\n"
        "\n"
        "```\n"
        "random code\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.proposals.size() == 1, "no-leak: one proposal");
    check(r.proposals[0].title == "First", "no-leak: proposal title");
    check(r.vault_updates.empty(), "no-leak: no vault updates leaked");
    check(r.observations.empty(), "no-leak: no observations leaked");
}

static void test_multiple_lenient_blocks() {
    sui::quorum::OutputParser parser;
    std::string input =
        "## VAULT_UPDATE\n"
        "```\n"
        "path: knowledge/session.md\n"
        "content: |\n"
        "  session data\n"
        "```\n"
        "\n"
        "## OBSERVATION\n"
        "```\n"
        "title: Fill Anomaly\n"
        "tags: [mm-bot]\n"
        "content: |\n"
        "  observed anomaly\n"
        "```\n"
        "\n"
        "**PROPOSAL \xe2\x80\x94 Widen Spread**\n"
        "```\n"
        "title: Widen Spread\n"
        "requires_consensus_from: [operator]\n"
        "content: |\n"
        "  widen by 15%\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.vault_updates.size() == 1, "multi: one vault update");
    check(r.observations.size() == 1, "multi: one observation");
    check(r.proposals.size() == 1, "multi: one proposal");
    check(r.vault_updates[0].path == "knowledge/session.md", "multi: vault path");
    check(r.observations[0].title == "Fill Anomaly", "multi: observation title");
    check(r.proposals[0].title == "Widen Spread", "multi: proposal title");
}

// ─── language-tagged fences, content fallback, and field alias tests ─────────

static void test_language_tagged_fence_markdown() {
    sui::quorum::OutputParser parser;
    std::string input =
        "## VAULT_UPDATE\n"
        "```markdown\n"
        "# Latest Session\n"
        "Some analysis.\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.vault_updates.size() == 1, "lang-md: one vault update");
    check(r.vault_updates[0].content.find("Latest Session") != std::string::npos,
          "lang-md: content contains 'Latest Session'");
    check(r.vault_updates[0].content.find("analysis") != std::string::npos,
          "lang-md: content contains 'analysis'");
}

static void test_language_tagged_fence_sql() {
    sui::quorum::OutputParser parser;
    std::string input =
        "**OBSERVATION**\n"
        "```sql\n"
        "title: Query Results\n"
        "tags: [db]\n"
        "content: |\n"
        "  SELECT * FROM sessions\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.observations.size() == 1, "lang-sql: one observation");
    check(r.observations[0].title == "Query Results", "lang-sql: title == 'Query Results'");
}

static void test_language_tagged_fence_no_pending_type() {
    sui::quorum::OutputParser parser;
    std::string input =
        "```markdown\n"
        "# Just a code block\n"
        "No type header above.\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.vault_updates.empty(), "lang-no-type: no vault updates");
    check(r.proposals.empty(), "lang-no-type: no proposals");
    check(r.observations.empty(), "lang-no-type: no observations");
}

static void test_proposal_freetext_content() {
    sui::quorum::OutputParser parser;
    std::string input =
        "**PROPOSAL**\n"
        "```\n"
        "title: Restart mm-bot\n"
        "severity: CRITICAL\n"
        "---\n"
        "**Problem:** Session stuck.\n"
        "\n"
        "**Fix:**\n"
        "1. Check logs\n"
        "2. Restart\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.proposals.size() == 1, "prop-ft: one proposal");
    check(r.proposals[0].title == "Restart mm-bot", "prop-ft: title matches");
    check(!r.proposals[0].content.empty(), "prop-ft: content NOT empty");
    check(r.proposals[0].content.find("Problem") != std::string::npos,
          "prop-ft: content contains 'Problem'");
    check(r.proposals[0].content.find("Restart") != std::string::npos,
          "prop-ft: content contains 'Restart'");
}

static void test_observation_freetext_content() {
    sui::quorum::OutputParser parser;
    std::string input =
        "## OBSERVATION\n"
        "```\n"
        "title: Fill Pattern\n"
        "tags: [mm-bot]\n"
        "---\n"
        "Clustering of fills at epoch boundaries.\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.observations.size() == 1, "obs-ft: one observation");
    check(r.observations[0].title == "Fill Pattern", "obs-ft: title matches");
    check(!r.observations[0].content.empty(), "obs-ft: content NOT empty");
    check(r.observations[0].content.find("Clustering") != std::string::npos,
          "obs-ft: content contains 'Clustering'");
}

static void test_vault_update_no_content_field() {
    sui::quorum::OutputParser parser;
    std::string input =
        "## VAULT_UPDATE\n"
        "```markdown\n"
        "# Market State\n"
        "\n"
        "DEEP/SUI at 0.0421.\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.vault_updates.size() == 1, "vu-no-content: one vault update");
    check(!r.vault_updates[0].content.empty(), "vu-no-content: content NOT empty");
    check(r.vault_updates[0].content.find("Market State") != std::string::npos,
          "vu-no-content: content contains 'Market State'");
    check(r.vault_updates[0].content.find("0.0421") != std::string::npos,
          "vu-no-content: content contains '0.0421'");
}

static void test_required_reviewers_alias() {
    sui::quorum::OutputParser parser;
    std::string input =
        "```PROPOSAL\n"
        "title: Widen Spread\n"
        "required_reviewers: [bot_analyst, engineer]\n"
        "content: |\n"
        "  Increase spread\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.proposals.size() == 1, "alias-rr: one proposal");
    check(r.proposals[0].requires_consensus_from.size() == 2,
          "alias-rr: two reviewers");
    check(r.proposals[0].requires_consensus_from[0] == "bot_analyst",
          "alias-rr: reviewer[0] == bot_analyst");
}

static void test_reviewers_alias() {
    sui::quorum::OutputParser parser;
    std::string input =
        "```PROPOSAL\n"
        "title: Reduce Size\n"
        "reviewers: [operator]\n"
        "content: |\n"
        "  Decrease size\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.proposals.size() == 1, "alias-rev: one proposal");
    check(r.proposals[0].requires_consensus_from.size() == 1,
          "alias-rev: one reviewer");
    check(r.proposals[0].requires_consensus_from[0] == "operator",
          "alias-rev: reviewer[0] == operator");
}

static void test_explicit_content_not_overridden() {
    sui::quorum::OutputParser parser;
    std::string input =
        "```PROPOSAL\n"
        "title: Test\n"
        "requires_consensus_from: [x]\n"
        "content: |\n"
        "  Explicit content here.\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.proposals.size() == 1, "no-override: one proposal");
    check(r.proposals[0].content == "Explicit content here.",
          "no-override: content == 'Explicit content here.'");
}

static void test_realistic_agent_output() {
    sui::quorum::OutputParser parser;
    std::string input =
        "## VAULT_UPDATE\n"
        "\n"
        "```markdown\n"
        "# System Health Report\n"
        "\n"
        "## mm-bot Status\n"
        "- PID 12345\n"
        "- Session #31\n"
        "- Fills: 0\n"
        "```\n"
        "\n"
        "## PROPOSAL\n"
        "\n"
        "```\n"
        "title: Investigate zero-fill session\n"
        "required_reviewers: [bot_analyst]\n"
        "severity: HIGH\n"
        "---\n"
        "Session #31: 9.5 hours, zero fills.\n"
        "\n"
        "Recommended:\n"
        "1. Check logs\n"
        "2. Verify liquidity\n"
        "3. Restart\n"
        "```\n"
        "\n"
        "## OBSERVATION\n"
        "\n"
        "```\n"
        "title: Zero-Fill Anomaly\n"
        "tags: [mm-bot, health, anomaly]\n"
        "---\n"
        "0 fills over 9.5 hours. Average is 40-60.\n"
        "```\n";
    auto r = parser.parse(input);

    // VAULT_UPDATE
    check(r.vault_updates.size() == 1, "realistic: one vault update");
    check(r.vault_updates[0].content.find("Health Report") != std::string::npos,
          "realistic: vault content contains 'Health Report'");

    // PROPOSAL
    check(r.proposals.size() == 1, "realistic: one proposal");
    check(r.proposals[0].title == "Investigate zero-fill session",
          "realistic: proposal title matches");
    check(r.proposals[0].requires_consensus_from.size() == 1,
          "realistic: one reviewer");
    check(r.proposals[0].requires_consensus_from[0] == "bot_analyst",
          "realistic: reviewer == bot_analyst");
    check(!r.proposals[0].content.empty(), "realistic: proposal content NOT empty");
    check(r.proposals[0].content.find("Recommended") != std::string::npos,
          "realistic: proposal content contains 'Recommended'");

    // OBSERVATION
    check(r.observations.size() == 1, "realistic: one observation");
    check(r.observations[0].title == "Zero-Fill Anomaly",
          "realistic: observation title matches");
    check(r.observations[0].tags.size() == 3, "realistic: 3 tags");
    check(!r.observations[0].content.empty(), "realistic: observation content NOT empty");
    check(r.observations[0].content.find("40-60") != std::string::npos,
          "realistic: observation content contains '40-60'");
}

// ─── VAULT_UPDATE path alias and bold metadata tests ────────────────────────

static void test_vault_path_target_alias() {
    sui::quorum::OutputParser parser;
    std::string input =
        "```VAULT_UPDATE\n"
        "target: knowledge/health.md\n"
        "content: |\n"
        "  status ok\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.vault_updates.size() == 1, "target-alias: one vault update");
    check(r.vault_updates[0].path == "knowledge/health.md",
          "target-alias: path == 'knowledge/health.md'");
}

static void test_vault_path_file_alias() {
    sui::quorum::OutputParser parser;
    std::string input =
        "```VAULT_UPDATE\n"
        "file: knowledge/report.md\n"
        "content: |\n"
        "  report\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.vault_updates.size() == 1, "file-alias: one vault update");
    check(r.vault_updates[0].path == "knowledge/report.md",
          "file-alias: path == 'knowledge/report.md'");
}

static void test_vault_path_bold_target_above() {
    sui::quorum::OutputParser parser;
    std::string input =
        "## VAULT_UPDATE\n"
        "\n"
        "**target:** knowledge/market_state.md\n"
        "\n"
        "```\n"
        "# Market State\n"
        "DEEP at 0.04\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.vault_updates.size() == 1, "bold-target: one vault update");
    check(r.vault_updates[0].path == "knowledge/market_state.md",
          "bold-target: path == 'knowledge/market_state.md'");
    check(r.vault_updates[0].content.find("Market State") != std::string::npos,
          "bold-target: content contains 'Market State'");
}

static void test_vault_path_bold_file_backtick_above() {
    sui::quorum::OutputParser parser;
    std::string input =
        "## VAULT_UPDATE\n"
        "\n"
        "**file:** `knowledge/latest_session.md`\n"
        "\n"
        "```markdown\n"
        "## Session 30\n"
        "Analysis here.\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.vault_updates.size() == 1, "bold-file-bt: one vault update");
    check(r.vault_updates[0].path == "knowledge/latest_session.md",
          "bold-file-bt: path == 'knowledge/latest_session.md'");
    check(r.vault_updates[0].content.find("Session 30") != std::string::npos,
          "bold-file-bt: content contains 'Session 30'");
}

static void test_vault_path_inside_overrides_above() {
    sui::quorum::OutputParser parser;
    std::string input =
        "## VAULT_UPDATE\n"
        "\n"
        "**target:** knowledge/old.md\n"
        "\n"
        "```\n"
        "path: knowledge/new.md\n"
        "content: |\n"
        "  data\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.vault_updates.size() == 1, "override: one vault update");
    check(r.vault_updates[0].path == "knowledge/new.md",
          "override: path == 'knowledge/new.md' (inside overrides above)");
}

static void test_vault_path_no_leak_to_non_vault() {
    sui::quorum::OutputParser parser;
    std::string input =
        "## PROPOSAL\n"
        "\n"
        "**target:** knowledge/foo.md\n"
        "\n"
        "```\n"
        "title: My Proposal\n"
        "content: |\n"
        "  stuff\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.proposals.size() == 1, "no-leak-non-vu: one proposal");
    check(r.proposals[0].title == "My Proposal",
          "no-leak-non-vu: title == 'My Proposal'");
}

static void test_vault_path_reset_across_blocks() {
    sui::quorum::OutputParser parser;
    std::string input =
        "## VAULT_UPDATE\n"
        "**target:** knowledge/a.md\n"
        "```\n"
        "# First\n"
        "```\n"
        "\n"
        "## VAULT_UPDATE\n"
        "```\n"
        "# Second\n"
        "```\n";
    auto r = parser.parse(input);
    check(r.vault_updates.size() == 2, "reset: two vault updates");
    check(r.vault_updates[0].path == "knowledge/a.md",
          "reset: first path == 'knowledge/a.md'");
    check(r.vault_updates[1].path.empty(),
          "reset: second path is empty (pending_path cleared)");
}

// ─── verdict normalization tests ─────────────────────────────────────────────

static void test_verdict_normalization() {
    sui::quorum::OutputParser parser;

    auto make_review = [&](const std::string& verdict_str) -> std::string {
        return "```REVIEW\n"
               "proposal_id: test-001\n"
               "verdict: " + verdict_str + "\n"
               "reasoning: test reasoning\n"
               "```\n";
    };

    // approve aliases
    auto r1 = parser.parse(make_review("approve"));
    check(r1.reviews[0].verdict == "approve", "verdict: approve -> approve");
    auto r2 = parser.parse(make_review("Approved"));
    check(r2.reviews[0].verdict == "approve", "verdict: Approved -> approve");
    auto r3 = parser.parse(make_review("ACCEPT"));
    check(r3.reviews[0].verdict == "approve", "verdict: ACCEPT -> approve");

    // reject aliases
    auto r4 = parser.parse(make_review("reject"));
    check(r4.reviews[0].verdict == "reject", "verdict: reject -> reject");
    auto r5 = parser.parse(make_review("Rejected"));
    check(r5.reviews[0].verdict == "reject", "verdict: Rejected -> reject");
    auto r6 = parser.parse(make_review("DENIED"));
    check(r6.reviews[0].verdict == "reject", "verdict: DENIED -> reject");

    // revise aliases
    auto r7 = parser.parse(make_review("revise"));
    check(r7.reviews[0].verdict == "revise", "verdict: revise -> revise");
    auto r8 = parser.parse(make_review("Revision"));
    check(r8.reviews[0].verdict == "revise", "verdict: Revision -> revise");
    auto r9 = parser.parse(make_review("needs_revision"));
    check(r9.reviews[0].verdict == "revise", "verdict: needs_revision -> revise");

    // escalate aliases
    auto r10 = parser.parse(make_review("escalate"));
    check(r10.reviews[0].verdict == "escalate", "verdict: escalate -> escalate");
    auto r11 = parser.parse(make_review("Escalated"));
    check(r11.reviews[0].verdict == "escalate", "verdict: Escalated -> escalate");
    auto r12 = parser.parse(make_review("needs_human"));
    check(r12.reviews[0].verdict == "escalate", "verdict: needs_human -> escalate");
    auto r13 = parser.parse(make_review("uncertain"));
    check(r13.reviews[0].verdict == "escalate", "verdict: uncertain -> escalate");

    // unknown -> reject (safe default)
    auto r14 = parser.parse(make_review("gibberish"));
    check(r14.reviews[0].verdict == "reject", "verdict: gibberish -> reject");

    // empty verdict -> reject
    auto r15 = parser.parse("```REVIEW\nproposal_id: test-001\nreasoning: no verdict\n```\n");
    check(r15.reviews[0].verdict == "reject", "verdict: (empty) -> reject");

    // whitespace handling
    auto r16 = parser.parse(make_review("  approve  "));
    check(r16.reviews[0].verdict == "approve", "verdict: '  approve  ' -> approve");
}

// ─── main ────────────────────────────────────────────────────────────────────

// ─── Regression: nested code fence inside content must NOT close the block ────
//
// Repro of the thinker broker-note truncation. A VAULT_UPDATE whose `content: |`
// literal embeds a fenced code block (an ASCII diagram). The inner ``` fences
// are indented two spaces under the literal; the parser must indent-match the
// CLOSING fence to the opening fence (column 0) so the inner fences are kept as
// content — and the trailing OBSERVATION block is parsed separately instead of
// being swallowed. Before the fix, the first inner ``` closed the block and
// everything after it was lost.
static void test_nested_code_fence_in_content() {
    sui::quorum::OutputParser parser;
    std::string input = R"DOC(```VAULT_UPDATE
path: knowledge/broker.md
content: |
  # Section 2 before the diagram

  ## 3. End-to-end path

  ```
  Broker -> HTTP POST /order
    -> verify -> engine
  ```

  ## 4. After the diagram
  This text comes AFTER the inner fence.
```

```OBSERVATION
title: Trailing observation
tags: [drift]
content: |
  This observation must be parsed separately.
```
)DOC";
    auto r = parser.parse(input);

    check(r.vault_updates.size() == 1, "nested-fence: exactly one vault update");
    const auto& c = r.vault_updates[0].content;
    check(c.find("Section 2 before the diagram") != std::string::npos,
          "nested-fence: content kept text BEFORE the inner fence");
    check(c.find("Broker -> HTTP POST /order") != std::string::npos,
          "nested-fence: content kept the inner code-block body");
    check(c.find("After the diagram") != std::string::npos,
          "nested-fence: content kept text AFTER the inner fence (no early close)");
    check(c.find("This text comes AFTER the inner fence") != std::string::npos,
          "nested-fence: full post-diagram prose preserved");
    check(c.find("```") != std::string::npos,
          "nested-fence: inner fence markers preserved in content");
    check(r.observations.size() == 1,
          "nested-fence: trailing OBSERVATION parsed separately (not swallowed)");
    check(r.observations.size() == 1 &&
          r.observations[0].title == "Trailing observation",
          "nested-fence: trailing observation title correct");
}

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

    // Lenient block detection tests
    test_heading_vault_update();
    test_bold_proposal();
    test_first_line_type_fallback();
    test_bold_with_type_inside_dedup();
    test_heading_with_suffix();
    test_bold_with_colon_suffix();
    test_backward_compat_explicit_fence();
    test_plain_fence_no_type_ignored();
    test_no_type_leak_across_blocks();
    test_multiple_lenient_blocks();

    // Language-tagged fences, content fallback, and field alias tests
    test_language_tagged_fence_markdown();
    test_language_tagged_fence_sql();
    test_language_tagged_fence_no_pending_type();
    test_proposal_freetext_content();
    test_observation_freetext_content();
    test_vault_update_no_content_field();
    test_required_reviewers_alias();
    test_reviewers_alias();
    test_explicit_content_not_overridden();
    test_realistic_agent_output();

    // VAULT_UPDATE path alias and bold metadata tests
    test_vault_path_target_alias();
    test_vault_path_file_alias();
    test_vault_path_bold_target_above();
    test_vault_path_bold_file_backtick_above();
    test_vault_path_inside_overrides_above();
    test_vault_path_no_leak_to_non_vault();
    test_vault_path_reset_across_blocks();

    // Verdict normalization tests
    test_verdict_normalization();

    // Regression: nested code fence inside VAULT_UPDATE content (truncation bug)
    test_nested_code_fence_in_content();

    std::cout << "\nAll tests passed.\n";
    return 0;
}
