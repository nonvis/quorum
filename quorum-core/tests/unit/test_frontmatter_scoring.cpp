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

    // Score signal:
    //   ref-tagged.md       → tag_hits=1 (×5) + content_hits=1 (the literal
    //                         "walrus" in the frontmatter line itself, since
    //                         content scanning is whole-file) = 6
    //   ref-walrus-name.md  → fn_hits=1 (×3) = 3
    // Ordering invariant 6 > 3 still holds (the spec's example arithmetic
    // ignored content_hits inside frontmatter; actual scorer counts it).
    check(split.user_message.find("score: 6") != std::string::npos,
          "T6: tagged ref reports score: 6 (5 tag + 1 content from frontmatter)");
    check(split.user_message.find("score: 3") != std::string::npos,
          "T6: filename-only ref reports score: 3 (filename match only)");

    cleanup(vault);
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

    std::cout << "\n---------------------------------------------------\n";
    std::cout << "  passed: " << g_passed << "  failed: " << g_failed << "\n";
    std::cout << "---------------------------------------------------\n";

    return g_failed == 0 ? 0 : 1;
}
