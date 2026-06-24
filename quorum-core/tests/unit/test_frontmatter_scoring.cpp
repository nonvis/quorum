// tests/unit/test_frontmatter_scoring.cpp
// Phase 9 Track 2 — frontmatter tag parsing + tag-weighted ref scoring.
//
// Verifies:
//   - parse_frontmatter_tags() handles valid + malformed + missing inputs.
//   - search_references() weights tag-exact matches at ×5 (above filename ×3
//     and content ×1) so a tagged ref outranks a filename-only ref.
//   - Untagged refs preserve Phase 8 ranking exactly (tag_hits=0 contributes
//     nothing) — backwards-compat invariant.
//
// Run: ctest -R test_frontmatter_scoring --output-on-failure

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <unistd.h>

#include "agent/context_assembler.h"
#include "utils/frontmatter.h"

namespace fs = std::filesystem;

static int g_passed = 0;
static int g_failed = 0;
static int g_test_num = 0;

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        ++g_failed;
        std::exit(1);
    }
    std::cout << "[PASS] " << msg << "\n";
    ++g_passed;
}

static std::string make_temp_vault() {
    auto dir = fs::temp_directory_path() /
        ("quorum_test_fm_" + std::to_string(getpid()) + "_" +
         std::to_string(g_test_num++));
    fs::create_directories(dir);
    fs::create_directories(dir / "knowledge");
    fs::create_directories(dir / "inbox");
    return dir.string();
}

static void cleanup(const std::string& path) {
    fs::remove_all(path);
}

static void write_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path);
    out << content;
}

static void write_knowledge(const std::string& vault, const std::string& name,
                            const std::string& content) {
    write_file(fs::path(vault) / "knowledge" / name, content);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
}

// ---------------------------------------------------------------------------
// Parser unit tests
// ---------------------------------------------------------------------------

static void test_t1_valid_tags() {
    std::cout << "\n=== T1. parse: valid tags: [walrus, seal] ===\n\n";
    std::string content = "---\ntags: [walrus, seal]\n---\n# body\n";
    auto tags = sui::quorum::parse_frontmatter_tags(content);
    check(tags.size() == 2, "T1: returns 2 tags");
    check(tags[0] == "walrus", "T1: tag[0] == walrus");
    check(tags[1] == "seal", "T1: tag[1] == seal");
}

static void test_t2_missing_close() {
    std::cout << "\n=== T2. parse: missing closing --- ===\n\n";
    std::string content = "---\ntags: [walrus]\n# body without close\n";
    auto tags = sui::quorum::parse_frontmatter_tags(content);
    check(tags.empty(), "T2: missing closing fence → {}");
}

static void test_t3_no_frontmatter() {
    std::cout << "\n=== T3. parse: no frontmatter at all ===\n\n";
    std::string content = "# Just a heading\n\nSome body text.\n";
    auto tags = sui::quorum::parse_frontmatter_tags(content);
    check(tags.empty(), "T3: no frontmatter → {}");
}

static void test_t4_malformed() {
    std::cout << "\n=== T4. parse: malformed `tags: [foo,` (no closing ]) ===\n\n";
    std::string content = "---\ntags: [foo,\n---\n# body\n";
    auto tags = sui::quorum::parse_frontmatter_tags(content);
    check(tags.empty(), "T4: malformed bracket → {}");
}

static void test_t5_mixed_case() {
    std::cout << "\n=== T5. parse: mixed-case → lowercase ===\n\n";
    std::string content = "---\ntags: [Walrus, SEAL, MoVe]\n---\nbody\n";
    auto tags = sui::quorum::parse_frontmatter_tags(content);
    check(tags.size() == 3, "T5: 3 tags returned");
    check(tags[0] == "walrus", "T5: 'Walrus' → 'walrus'");
    check(tags[1] == "seal", "T5: 'SEAL' → 'seal'");
    check(tags[2] == "move", "T5: 'MoVe' → 'move'");
}

// ---------------------------------------------------------------------------
// T6: tag-match outranks filename-match (covers plan #9)
// ---------------------------------------------------------------------------

static void test_t6_tag_outranks_filename() {
    std::cout << "\n=== T6. tag-match (×5) outranks filename-match (×3) ===\n\n";

    auto vault = make_temp_vault();

    // ref-tagged.md: frontmatter tags [walrus]; body has no query tokens.
    write_knowledge(vault, "ref-tagged.md",
        "---\ntags: [walrus]\n---\n# Tagged note\n\nirrelevant body content here.\n");
    // ref-walrus-name.md: no frontmatter; filename has "walrus"; body is
    // unrelated.
    write_knowledge(vault, "ref-walrus-name.md",
        "# Untagged note\n\nThis body has nothing relevant.\n");

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "agent-t6", vault, "turn", "walrus storage");

    // Refs surface in '## Searched References'. Both should score > 0,
    // ref-tagged ranked above ref-walrus-name.
    auto pos_section = split.user_message.find("## Searched References");
    check(pos_section != std::string::npos,
          "T6: '## Searched References' section present");

    auto pos_tagged = split.user_message.find("ref-tagged.md", pos_section);
    auto pos_filename_only = split.user_message.find("ref-walrus-name.md", pos_section);
    check(pos_tagged != std::string::npos,
          "T6: ref-tagged.md surfaced");
    check(pos_filename_only != std::string::npos,
          "T6: ref-walrus-name.md surfaced");
    check(pos_tagged < pos_filename_only,
          "T6: ref-tagged.md ranked ABOVE ref-walrus-name.md");

    // Score signal (post-#27d: frontmatter stripped from content scoring):
    //   ref-tagged.md       → tag_hits=1 (×5) + content_hits=0 = 5
    //   ref-walrus-name.md  → fn_hits=1 (×3) = 3
    // Ordering 5 > 3 holds; no double-counting of tag words via frontmatter.
    check(split.user_message.find("score: 5") != std::string::npos,
          "T6: tagged ref reports score: 5 (5 tag + 0 content; #27d)");
    check(split.user_message.find("score: 3") != std::string::npos,
          "T6: filename-only ref reports score: 3 (filename match only)");

    cleanup(vault);
}

// ---------------------------------------------------------------------------
// T8: #27d regression — frontmatter words don't double-count in content score
// ---------------------------------------------------------------------------

static void test_t8_frontmatter_no_double_count() {
    std::cout << "\n=== T8. #27d: frontmatter words excluded from content scoring ===\n\n";

    auto vault = make_temp_vault();

    // ref-just-tag.md: query word "walrus" appears ONLY in frontmatter.
    //   Pre-fix: tag_hits=1 (×5) + content_hits=1 = 6
    //   Post-fix: tag_hits=1 (×5) + content_hits=0 = 5
    write_knowledge(vault, "ref-just-tag.md",
        "---\ntags: [walrus]\n---\n# Note\n\nNo relevant words in body.\n");

    // ref-body-only.md: query word "walrus" appears ONLY in body, untagged.
    //   tag_hits=0, content_hits=1, fn_hits=0 → score 1.
    write_knowledge(vault, "ref-body-only.md",
        "# Other note\n\nBody mentions walrus once here.\n");

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "agent-t8", vault, "turn", "walrus");

    auto pos_section = split.user_message.find("## Searched References");
    check(pos_section != std::string::npos,
          "T8: '## Searched References' section present");
    check(split.user_message.find("score: 5") != std::string::npos,
          "T8: tag-only ref scores 5 (no content-side double-count)");
    check(split.user_message.find("score: 1") != std::string::npos,
          "T8: body-only ref scores 1 (single content hit)");
    // Negative: NEITHER ref should report score: 6 anywhere — that would mean
    // the frontmatter word leaked into content_hits.
    check(split.user_message.find("score: 6") == std::string::npos,
          "T8: no score: 6 anywhere (frontmatter not double-counted)");

    cleanup(vault);
}

// ---------------------------------------------------------------------------
// T9: frontmatter `summary:` becomes the excerpt verbatim (write-for-retrieval)
// ---------------------------------------------------------------------------

static void test_t9_summary_becomes_excerpt() {
    std::cout << "\n=== T9. frontmatter summary: overrides body-scrape excerpt ===\n\n";

    auto vault = make_temp_vault();

    // ref-walrus-guide.md: body LEADS with a heading + table (the body-scrape
    // path would surface "Heading"). Frontmatter carries a distinctive summary
    // sentence + a query-matching tag. The summary must win the preview.
    write_knowledge(vault, "ref-walrus-guide.md",
        "---\ntags: [walrus]\nsummary: WHENTOCHOOSE walrus blob lifecycle decision guide.\n---\n"
        "# Heading\n\n| col | col |\n|---|---|\n| a | b |\n");

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "agent-t9", vault, "turn", "walrus");

    auto pos_section = split.user_message.find("## Searched References");
    check(pos_section != std::string::npos,
          "T9: '## Searched References' section present");
    auto refs = split.user_message.substr(pos_section);

    check(refs.find("ref-walrus-guide.md") != std::string::npos,
          "T9: ref-walrus-guide.md surfaced");
    check(refs.find("WHENTOCHOOSE") != std::string::npos,
          "T9: summary sentence shown verbatim as excerpt");
    check(refs.find("Heading") == std::string::npos,
          "T9: body heading NOT scraped (summary overrode body-scrape)");

    cleanup(vault);
}

// ---------------------------------------------------------------------------
// T10: no summary → body-scrape fallback (make_excerpt(body)) still fires
// ---------------------------------------------------------------------------

static void test_t10_no_summary_body_fallback() {
    std::cout << "\n=== T10. no summary: → make_excerpt(body) fallback ===\n\n";

    auto vault = make_temp_vault();

    // ref-walrus-body.md: tagged for the query but NO summary field. The body's
    // first words are distinctive — the body-scrape path must surface them.
    write_knowledge(vault, "ref-walrus-body.md",
        "---\ntags: [walrus]\n---\n# Title\n\nBODYFIRSTWORDS describe walrus storage here.\n");

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "agent-t10", vault, "turn", "walrus");

    auto pos_section = split.user_message.find("## Searched References");
    check(pos_section != std::string::npos,
          "T10: '## Searched References' section present");
    auto refs = split.user_message.substr(pos_section);

    check(refs.find("ref-walrus-body.md") != std::string::npos,
          "T10: ref-walrus-body.md surfaced");
    check(refs.find("BODYFIRSTWORDS") != std::string::npos,
          "T10: body-scrape excerpt fired (no summary present)");

    cleanup(vault);
}

// ---------------------------------------------------------------------------
// T11: malformed frontmatter → fail-closed, no crash, summary not leaked
// ---------------------------------------------------------------------------

static void test_t11_malformed_summary_fail_closed() {
    std::cout << "\n=== T11. malformed summary → fail-closed fallback (no crash) ===\n\n";

    auto vault = make_temp_vault();

    // ref-walrus-fallback.md: frontmatter fence is well-formed but carries only
    // tags — the `summary:` key sits OUTSIDE the fence (down in the body), so
    // parse_frontmatter_field() returns "" (fail-closed) and the body-scrape
    // fallback fires. The query token lives in the FILENAME so the ref still
    // surfaces (filename match, score > 0). The stray BROKENSUMMARY token is
    // pushed past the 200-char excerpt cap by leading filler, so it must NOT
    // appear in the rendered preview.
    write_knowledge(vault, "ref-walrus-fallback.md",
        "---\ntags: [unrelated]\n---\n"
        "LEADFILLER lorem ipsum dolor sit amet consectetur adipiscing elit sed do "
        "eiusmod tempor incididunt ut labore et dolore magna aliqua ut enim ad minim "
        "veniam quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo "
        "consequat duis aute irure dolor in reprehenderit.\n"
        "summary: BROKENSUMMARY this value is outside the fence and must be ignored.\n");

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "agent-t11", vault, "turn", "walrus");

    auto pos_section = split.user_message.find("## Searched References");
    check(pos_section != std::string::npos,
          "T11: '## Searched References' section present (no crash)");
    auto refs = split.user_message.substr(pos_section);

    check(refs.find("ref-walrus-fallback.md") != std::string::npos,
          "T11: ref-walrus-fallback.md surfaced via filename match");
    check(refs.find("BROKENSUMMARY") == std::string::npos,
          "T11: malformed/out-of-fence summary value NOT shown as excerpt");

    cleanup(vault);
}

// ---------------------------------------------------------------------------
// T12: parse_frontmatter_field — YAML block scalars (>/|) folded to one line
// ---------------------------------------------------------------------------

static void test_t12_block_scalars() {
    std::cout << "\n=== T12. parse_frontmatter_field: block scalars (>/|) ===\n\n";
    using sui::quorum::parse_frontmatter_field;

    // (a) folded `>` across multiple indented lines → space-joined, no leading '>'.
    {
        std::string c =
            "---\n"
            "summary: >\n"
            "  Long folded summary across\n"
            "  multiple indented lines.\n"
            "---\nbody\n";
        auto v = parse_frontmatter_field(c, "summary");
        check(v == "Long folded summary across multiple indented lines.",
              "T12a: folded '>' joined by single spaces, no leading '>'");
    }

    // (b) literal `|` multi-line → joined (space-join acceptable per single-line
    //     preview contract).
    {
        std::string c =
            "---\n"
            "summary: |\n"
            "  line one\n"
            "  line two\n"
            "  line three\n"
            "---\nbody\n";
        auto v = parse_frontmatter_field(c, "summary");
        check(v == "line one line two line three",
              "T12b: literal '|' folded to space-joined single line");
    }

    // (c) chomping indicators on the header → still a block.
    {
        std::string c =
            "---\n"
            "summary: >-\n"
            "  folded with strip chomp\n"
            "  second line\n"
            "---\nbody\n";
        auto v = parse_frontmatter_field(c, "summary");
        check(v == "folded with strip chomp second line",
              "T12c1: '>-' parsed as block");

        std::string c2 =
            "---\n"
            "summary: |+\n"
            "  literal keep chomp\n"
            "---\nbody\n";
        auto v2 = parse_frontmatter_field(c2, "summary");
        check(v2 == "literal keep chomp",
              "T12c2: '|+' parsed as block");
    }

    // (d) block does NOT bleed into the next sibling key; sibling still parses.
    {
        std::string c =
            "---\n"
            "summary: >\n"
            "  folded body here\n"
            "  more body\n"
            "tags: [walrus, seal]\n"
            "---\nbody\n";
        auto v = parse_frontmatter_field(c, "summary");
        check(v == "folded body here more body",
              "T12d1: block stops before sibling key (no bleed)");
        check(parse_frontmatter_field(c, "tags") == "[walrus, seal]",
              "T12d2: sibling 'tags' single-line value still parsed");
        auto tags = sui::quorum::parse_frontmatter_tags(c);
        check(tags.size() == 2 && tags[0] == "walrus" && tags[1] == "seal",
              "T12d3: parse_frontmatter_tags unaffected by preceding block");

        // sibling that is itself a plain key:
        std::string c2 =
            "---\n"
            "summary: >\n"
            "  folded\n"
            "foo: bar\n"
            "---\nbody\n";
        check(parse_frontmatter_field(c2, "summary") == "folded",
              "T12d4: block stops before 'foo:' sibling");
        check(parse_frontmatter_field(c2, "foo") == "bar",
              "T12d5: 'foo' sibling value parsed after block");
    }

    // (e) block terminated by the closing '---' fence → no '---' in value.
    {
        std::string c =
            "---\n"
            "summary: >\n"
            "  text before the fence\n"
            "---\nbody\n";
        auto v = parse_frontmatter_field(c, "summary");
        check(v == "text before the fence",
              "T12e: block ends at '---' fence, no fence in value");
        check(v.find("---") == std::string::npos,
              "T12e2: closing fence not leaked into value");
    }

    // (f) REGRESSION — single-line / quoted / pseudo-block values unchanged.
    {
        check(parse_frontmatter_field(
                  "---\nsummary: Plain text\n---\nb\n", "summary") == "Plain text",
              "T12f1: plain single-line value unchanged");
        check(parse_frontmatter_field(
                  "---\nsummary: \"Quoted\"\n---\nb\n", "summary") == "Quoted",
              "T12f2: quoted value unchanged (quotes stripped)");
        // '>' followed by real text on the SAME line is NOT a block header.
        check(parse_frontmatter_field(
                  "---\nsummary: > not a block\n---\nb\n", "summary") == "> not a block",
              "T12f3: '> text' on same line is a plain value (not a block)");
        // '|pipe' — pipe immediately followed by letters → not a block.
        check(parse_frontmatter_field(
                  "---\nsummary: |pipe\n---\nb\n", "summary") == "|pipe",
              "T12f4: '|pipe' is a plain value (not a block)");
        // '>= 5' → not a block (contains '=' and space-text).
        check(parse_frontmatter_field(
                  "---\nsummary: >= 5\n---\nb\n", "summary") == ">= 5",
              "T12f5: '>= 5' is a plain value (not a block)");
    }

    // (g) empty block — header followed immediately by fence or sibling → "".
    {
        check(parse_frontmatter_field(
                  "---\nsummary: >\n---\nbody\n", "summary").empty(),
              "T12g1: empty block before fence → \"\"");
        check(parse_frontmatter_field(
                  "---\nsummary: |\ntags: [x]\n---\nbody\n", "summary").empty(),
              "T12g2: empty block before sibling key → \"\"");
    }
}

// ---------------------------------------------------------------------------
// T13: strip_frontmatter unaffected by block scalars (regression guard)
// ---------------------------------------------------------------------------

static void test_t13_strip_frontmatter_with_block() {
    std::cout << "\n=== T13. strip_frontmatter: block-scalar body still stripped ===\n\n";
    std::string c =
        "---\n"
        "summary: >\n"
        "  folded summary line\n"
        "  second line\n"
        "tags: [walrus]\n"
        "---\n"
        "# Body Heading\n\nBody text.\n";
    auto stripped = sui::quorum::strip_frontmatter(c);
    check(stripped == "# Body Heading\n\nBody text.\n",
          "T13: frontmatter (incl. block body) stripped, body intact");
    check(stripped.find("folded summary") == std::string::npos,
          "T13: block-scalar body not leaked into stripped content");
}

// ---------------------------------------------------------------------------
// T7: untagged backwards-compat — Phase 8 ranking unchanged (covers plan #10)
// ---------------------------------------------------------------------------

static void test_t7_untagged_backcompat() {
    std::cout << "\n=== T7. untagged refs: Phase 8 ranking invariant ===\n\n";

    auto vault = make_temp_vault();

    // ref-deepbook.md:
    //   fn_hits     = "deepbook" in filename → 1
    //   content_hits = "deepbook" + "matching" in body → 2
    //   score = 1*3 + 0*5 + 2 = 5
    write_knowledge(vault, "ref-deepbook.md",
        "# DeepBook overview\n\nThis explains the matching engine architecture.\n");

    // ref-other.md:
    //   fn_hits     = 0
    //   content_hits = "matching" twice in body → 2 (no "deepbook" anywhere)
    //   score = 0 + 0 + 2 = 2
    write_knowledge(vault, "ref-other.md",
        "# Other notes\n\nGeneric matching trivia, more matching trivia here.\n");

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "agent-t7", vault, "turn", "deepbook matching");

    auto pos_section = split.user_message.find("## Searched References");
    check(pos_section != std::string::npos,
          "T7: '## Searched References' section present");

    auto pos_deepbook = split.user_message.find("ref-deepbook.md", pos_section);
    auto pos_other = split.user_message.find("ref-other.md", pos_section);
    check(pos_deepbook != std::string::npos, "T7: ref-deepbook.md surfaced");
    check(pos_other != std::string::npos, "T7: ref-other.md surfaced");
    check(pos_deepbook < pos_other,
          "T7: ref-deepbook.md ranked above ref-other.md (Phase 8 invariant)");

    // Exact pre-Track-2 scores (tag_hits=0 contributes nothing):
    //   ref-deepbook.md: fn_hits=1, content_hits=2 → 3 + 2 = 5
    //   ref-other.md:    fn_hits=0, content_hits=2 → 0 + 2 = 2
    // Identical to what Phase 8 produced — backwards-compat invariant.
    check(split.user_message.find("score: 5") != std::string::npos,
          "T7: ref-deepbook.md scores 5 (3 fn + 2 content)");
    check(split.user_message.find("score: 2") != std::string::npos,
          "T7: ref-other.md scores 2 (2 content)");

    cleanup(vault);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    std::cout << "=====================================================\n";
    std::cout << "  Phase 9 Track 2 — frontmatter parser + scoring tests\n";
    std::cout << "=====================================================\n";

    test_t1_valid_tags();
    test_t2_missing_close();
    test_t3_no_frontmatter();
    test_t4_malformed();
    test_t5_mixed_case();
    test_t6_tag_outranks_filename();
    test_t7_untagged_backcompat();
    test_t8_frontmatter_no_double_count();
    test_t9_summary_becomes_excerpt();
    test_t10_no_summary_body_fallback();
    test_t11_malformed_summary_fail_closed();
    test_t12_block_scalars();
    test_t13_strip_frontmatter_with_block();

    std::cout << "\n---------------------------------------------------\n";
    std::cout << "  passed: " << g_passed << "  failed: " << g_failed << "\n";
    std::cout << "---------------------------------------------------\n";

    return g_failed == 0 ? 0 : 1;
}
