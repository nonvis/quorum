// tests/unit/test_scribe_write_discipline.cpp
// Phase 10 Track 10 #52 - scribe write-discipline primitive
// (handoff protocol v0.1).
//
// Four assertions per parent plan:
//   (a) Bootstrap: .quorum/learnings.md created with canonical structure
//       on first write to a fresh project.
//   (b) Append-only: re-run preserves prior entries verbatim and appends
//       a new session block.
//   (c) Timestamp discipline: Created at: written once + immutable;
//       Updated at: refreshed per write.
//   (d) Non-canonical header rejection: validate_canonical_headers rejects
//       any heading not in the canonical five.
//
// Run:  cd build && ctest -R test_scribe_write_discipline --output-on-failure

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "agent/output_parser.h"
#include "vault/scribe_writer.h"

namespace fs = std::filesystem;

static int g_passed = 0;
static int g_failed = 0;

#define check(cond, msg) do {                                         \
    if (cond) { ++g_passed; std::cout << "  PASS: " << msg << "\n"; } \
    else      { ++g_failed; std::cerr << "  FAIL: " << msg << "\n"; } \
} while (0)

static std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Line-anchored heading count: matches `needle` only when it appears at the
// start of a line (start-of-file or preceded by '\n'). Used to count session
// headings without false-matching the spec's explanatory blockquote, which
// quotes "## Learnings, <UTC>" inside a code span. Mirrors the Sub-gate F
// runbook's `grep -c '^## Learnings,'` pattern.
static size_t count_line_anchored(const std::string& haystack,
                                  const std::string& needle) {
    if (needle.empty()) return 0;
    size_t count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        bool at_line_start = (pos == 0) || (haystack[pos - 1] == '\n');
        if (at_line_start) ++count;
        pos += needle.size();
    }
    return count;
}

// Extract the value of a single-line "Key: value" entry at the top of the
// file. Returns "" if the marker is absent.
static std::string extract_line_after(const std::string& content,
                                      const std::string& marker) {
    auto pos = content.find(marker);
    if (pos == std::string::npos) return {};
    pos += marker.size();
    auto eol = content.find('\n', pos);
    if (eol == std::string::npos) return content.substr(pos);
    return content.substr(pos, eol - pos);
}

// Loose ISO-8601 UTC sanity: yyyy-MM-ddThh:mm:ssZ shape.
static bool looks_like_iso_utc(const std::string& s) {
    if (s.size() != 20) return false;
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    return digit(s[0]) && digit(s[1]) && digit(s[2]) && digit(s[3]) &&
           s[4] == '-' && digit(s[5]) && digit(s[6]) &&
           s[7] == '-' && digit(s[8]) && digit(s[9]) &&
           s[10] == 'T' && digit(s[11]) && digit(s[12]) &&
           s[13] == ':' && digit(s[14]) && digit(s[15]) &&
           s[16] == ':' && digit(s[17]) && digit(s[18]) &&
           s[19] == 'Z';
}

// ---- Case A: bootstrap -----------------------------------------------------
static void test_A_bootstrap(const fs::path& tdir) {
    std::cout << "\n=== Case A: bootstrap creates canonical structure ===\n\n";

    auto proj = tdir / "A";
    fs::create_directories(proj);

    sui::quorum::ScribeLearningsEntry entry;
    entry.utc_timestamp = "2026-05-28T10:00:00Z";
    entry.tried = {"did X"};
    entry.worked = {"X worked"};
    // did_not_work / open_questions / decisions left empty - must be omitted.

    auto result = sui::quorum::apply_scribe_learnings_update(
        proj.string(), entry);
    check(result.ok, "A: result.ok");
    check(result.bootstrapped, "A: result.bootstrapped");

    auto learn = proj / ".quorum" / "learnings.md";
    check(fs::exists(learn), "A: .quorum/learnings.md exists");

    auto content = read_file(learn);
    check(content.find("# Quorum project learnings") != std::string::npos,
          "A: file header present");
    check(content.find("Created at: 2026-05-28T10:00:00Z") != std::string::npos,
          "A: Created at set to entry timestamp");
    check(content.find("Updated at: 2026-05-28T10:00:00Z") != std::string::npos,
          "A: Updated at set to entry timestamp");
    check(content.find("## Learnings, 2026-05-28T10:00:00Z") != std::string::npos,
          "A: session heading present");
    check(content.find("### What we tried") != std::string::npos,
          "A: tried heading present");
    check(content.find("- did X") != std::string::npos,
          "A: tried bullet present");
    check(content.find("### What worked") != std::string::npos,
          "A: worked heading present");
    check(content.find("- X worked") != std::string::npos,
          "A: worked bullet present");
    check(content.find("### What did not work") == std::string::npos,
          "A: empty did_not_work omitted");
    check(content.find("### Open questions") == std::string::npos,
          "A: empty open_questions omitted");
    check(content.find("### Decisions") == std::string::npos,
          "A: empty decisions omitted");
}

// ---- Case B: append-only across re-runs ------------------------------------
static void test_B_append_only(const fs::path& tdir) {
    std::cout << "\n=== Case B: re-run appends + preserves prior entries ===\n\n";

    auto proj = tdir / "B";
    fs::create_directories(proj);

    // First write (bootstrap).
    sui::quorum::ScribeLearningsEntry entry1;
    entry1.utc_timestamp = "2026-05-28T10:00:00Z";
    entry1.tried = {"did X"};
    entry1.worked = {"X worked"};
    auto r1 = sui::quorum::apply_scribe_learnings_update(proj.string(), entry1);
    check(r1.ok, "B: first write ok");
    check(r1.bootstrapped, "B: first write bootstrapped");

    // Second write (append).
    sui::quorum::ScribeLearningsEntry entry2;
    entry2.utc_timestamp = "2026-05-28T11:00:00Z";
    entry2.decisions = {"chose Y"};
    auto r2 = sui::quorum::apply_scribe_learnings_update(proj.string(), entry2);
    check(r2.ok, "B: second write ok");
    check(!r2.bootstrapped, "B: second write NOT bootstrapped (file existed)");

    auto learn = proj / ".quorum" / "learnings.md";
    auto content = read_file(learn);

    // Prior entry intact.
    check(content.find("- did X") != std::string::npos,
          "B: prior 'did X' bullet preserved");
    check(content.find("- X worked") != std::string::npos,
          "B: prior 'X worked' bullet preserved");
    check(content.find("## Learnings, 2026-05-28T10:00:00Z") != std::string::npos,
          "B: prior session heading preserved");

    // New entry appended.
    check(content.find("## Learnings, 2026-05-28T11:00:00Z") != std::string::npos,
          "B: new session heading appended");
    check(content.find("- chose Y") != std::string::npos,
          "B: new decisions bullet appended");

    // Exactly two `## Learnings,` headings at line-start. (The bootstrap
    // preamble's explanatory blockquote contains the literal text inside a
    // code span, so we anchor on line-start to avoid counting that.)
    auto session_count = count_line_anchored(content, "## Learnings,");
    check(session_count == 2,
          "B: exactly two `## Learnings,` headings present");
}

// ---- Case C: timestamp discipline ------------------------------------------
static void test_C_timestamps(const fs::path& tdir) {
    std::cout << "\n=== Case C: Created at: immutable, Updated at: refreshed ===\n\n";

    auto proj = tdir / "C";
    fs::create_directories(proj);

    sui::quorum::ScribeLearningsEntry entry1;
    entry1.utc_timestamp = "2026-05-28T09:00:00Z";
    entry1.tried = {"first attempt"};
    auto r1 = sui::quorum::apply_scribe_learnings_update(proj.string(), entry1);
    check(r1.ok, "C: first write ok");

    auto learn = proj / ".quorum" / "learnings.md";
    auto content_t1 = read_file(learn);

    auto created_t1 = extract_line_after(content_t1, "Created at: ");
    auto updated_t1 = extract_line_after(content_t1, "Updated at: ");
    check(looks_like_iso_utc(created_t1),
          "C: Created at: matches ISO-8601 UTC shape");
    check(looks_like_iso_utc(updated_t1),
          "C: Updated at: matches ISO-8601 UTC shape");
    check(updated_t1 == "2026-05-28T09:00:00Z",
          "C: Updated at == entry1 timestamp");
    check(created_t1 == "2026-05-28T09:00:00Z",
          "C: Created at == entry1 timestamp on bootstrap");

    // Second write with different timestamp.
    sui::quorum::ScribeLearningsEntry entry2;
    entry2.utc_timestamp = "2026-05-28T15:30:00Z";
    entry2.worked = {"second attempt worked"};
    auto r2 = sui::quorum::apply_scribe_learnings_update(proj.string(), entry2);
    check(r2.ok, "C: second write ok");

    auto content_t2 = read_file(learn);
    auto created_t2 = extract_line_after(content_t2, "Created at: ");
    auto updated_t2 = extract_line_after(content_t2, "Updated at: ");

    check(created_t2 == created_t1,
          "C: Created at: byte-identical across writes (immutable)");
    check(updated_t2 != updated_t1,
          "C: Updated at: changed across writes");
    check(updated_t2 == "2026-05-28T15:30:00Z",
          "C: Updated at: == entry2 timestamp on second write");
    check(looks_like_iso_utc(updated_t2),
          "C: Updated at: still matches ISO-8601 shape after refresh");
}

// ---- Case D: non-canonical header rejection --------------------------------
static void test_D_validate_headers() {
    std::cout << "\n=== Case D: validate_canonical_headers ===\n\n";

    // Non-canonical heading mixed in -> reject.
    std::vector<std::string> bad = {
        "What we tried",
        "Lessons learned",   // non-canonical
        "Decisions",
    };
    auto rbad = sui::quorum::validate_canonical_headers(bad);
    check(!rbad.ok, "D: mixed-canonical rejected");
    check(rbad.reason.find("non-canonical") != std::string::npos,
          "D: reason mentions 'non-canonical'");
    check(rbad.reason.find("Lessons learned") != std::string::npos,
          "D: reason names the offending heading");

    // All five canonical -> accept.
    std::vector<std::string> all_canonical = {
        "What we tried",
        "What worked",
        "What did not work",
        "Open questions",
        "Decisions",
    };
    auto rgood = sui::quorum::validate_canonical_headers(all_canonical);
    check(rgood.ok, "D: full canonical set accepted");

    // Subset (omitting some) -> accept (empty sub-sections may be omitted).
    std::vector<std::string> subset = {"What we tried", "Decisions"};
    auto rsubset = sui::quorum::validate_canonical_headers(subset);
    check(rsubset.ok, "D: subset of canonical headings accepted");

    // Empty list -> accept (trivially valid).
    std::vector<std::string> empty_list;
    auto rempty = sui::quorum::validate_canonical_headers(empty_list);
    check(rempty.ok, "D: empty heading list accepted");

    // Single non-canonical -> reject.
    std::vector<std::string> single_bad = {"Random unspecified heading"};
    auto rsingle = sui::quorum::validate_canonical_headers(single_bad);
    check(!rsingle.ok, "D: single non-canonical heading rejected");
    check(rsingle.reason.find("Random unspecified heading") != std::string::npos,
          "D: single-bad reason names the heading");
}

// ---- Case E: parser basic LEARNINGS_UPDATE parse ---------------------------
// Phase 10 Track 10 v0.2 - the parser must recognize a complete
// LEARNINGS_UPDATE block (all five sub-fields populated) and surface a
// single ScribeLearningsEntry on ParsedOutput.learnings_updates. The
// entry then drives apply_scribe_learnings_update end-to-end to confirm
// parser-to-primitive integration.
static void test_E_parser_basic(const fs::path& tdir) {
    std::cout << "\n=== Case E: parser parses well-formed LEARNINGS_UPDATE ===\n\n";

    const std::string raw =
        "```LEARNINGS_UPDATE\n"
        "utc: 2026-05-29T01:31:45Z\n"
        "tried: |\n"
        "  - did X\n"
        "  - tried Y\n"
        "worked: |\n"
        "  - X worked because Z\n"
        "did_not_work: |\n"
        "  - Y failed due to W\n"
        "open_questions: |\n"
        "  - is V resolvable?\n"
        "decisions: |\n"
        "  - chose X over Y because Z\n"
        "```\n";

    sui::quorum::OutputParser parser;
    auto parsed = parser.parse(raw);

    check(parsed.learnings_updates.size() == 1,
          "E: exactly one learnings_updates entry parsed");
    if (parsed.learnings_updates.size() != 1) return;

    const auto& e = parsed.learnings_updates[0];
    check(e.utc_timestamp == "2026-05-29T01:31:45Z",
          "E: utc_timestamp parsed");
    check(e.tried.size() == 2 && e.tried[0] == "did X" && e.tried[1] == "tried Y",
          "E: tried bullets parsed (2 entries)");
    check(e.worked.size() == 1 && e.worked[0] == "X worked because Z",
          "E: worked bullets parsed (1 entry)");
    check(e.did_not_work.size() == 1 && e.did_not_work[0] == "Y failed due to W",
          "E: did_not_work bullets parsed (1 entry)");
    check(e.open_questions.size() == 1 && e.open_questions[0] == "is V resolvable?",
          "E: open_questions bullets parsed (1 entry)");
    check(e.decisions.size() == 1 &&
              e.decisions[0] == "chose X over Y because Z",
          "E: decisions bullets parsed (1 entry)");

    // End-to-end: apply the parsed entry and assert the on-disk file
    // contains all five canonical sub-headings.
    auto proj = tdir / "E";
    fs::create_directories(proj);
    auto r = sui::quorum::apply_scribe_learnings_update(proj.string(), e);
    check(r.ok, "E: apply_scribe_learnings_update ok");
    check(r.bootstrapped, "E: bootstrap on fresh dir");

    auto content = read_file(proj / ".quorum" / "learnings.md");
    check(content.find("## Learnings, 2026-05-29T01:31:45Z") != std::string::npos,
          "E: session heading present on disk");
    check(content.find("### What we tried") != std::string::npos,
          "E: tried heading on disk");
    check(content.find("- did X") != std::string::npos,
          "E: tried bullet 1 on disk");
    check(content.find("- tried Y") != std::string::npos,
          "E: tried bullet 2 on disk");
    check(content.find("### What worked") != std::string::npos,
          "E: worked heading on disk");
    check(content.find("### What did not work") != std::string::npos,
          "E: did_not_work heading on disk");
    check(content.find("### Open questions") != std::string::npos,
          "E: open_questions heading on disk");
    check(content.find("### Decisions") != std::string::npos,
          "E: decisions heading on disk");
}

// ---- Case F: parser drops empty sub-fields ---------------------------------
// The scribe omits a sub-field line entirely when it has no bullets to
// emit. The parser must produce an entry whose unfilled vectors are
// empty, and the rendered on-disk file must skip the corresponding
// `### ...` sub-headings.
static void test_F_parser_empty_subfields_omitted(const fs::path& tdir) {
    std::cout << "\n=== Case F: parser handles omitted sub-fields ===\n\n";

    const std::string raw =
        "```LEARNINGS_UPDATE\n"
        "utc: 2026-05-29T02:00:00Z\n"
        "worked: |\n"
        "  - only thing that worked\n"
        "```\n";

    sui::quorum::OutputParser parser;
    auto parsed = parser.parse(raw);

    check(parsed.learnings_updates.size() == 1,
          "F: exactly one learnings_updates entry parsed");
    if (parsed.learnings_updates.size() != 1) return;

    const auto& e = parsed.learnings_updates[0];
    check(e.utc_timestamp == "2026-05-29T02:00:00Z", "F: utc parsed");
    check(e.tried.empty(),          "F: tried empty (sub-field omitted)");
    check(e.did_not_work.empty(),   "F: did_not_work empty");
    check(e.open_questions.empty(), "F: open_questions empty");
    check(e.decisions.empty(),      "F: decisions empty");
    check(e.worked.size() == 1 && e.worked[0] == "only thing that worked",
          "F: worked has the single bullet");

    // End-to-end on-disk check: only `### What worked` should appear.
    auto proj = tdir / "F";
    fs::create_directories(proj);
    auto r = sui::quorum::apply_scribe_learnings_update(proj.string(), e);
    check(r.ok, "F: apply_scribe_learnings_update ok");

    auto content = read_file(proj / ".quorum" / "learnings.md");
    check(content.find("### What worked") != std::string::npos,
          "F: worked heading present on disk");
    check(content.find("- only thing that worked") != std::string::npos,
          "F: worked bullet present on disk");
    check(content.find("### What we tried") == std::string::npos,
          "F: tried heading omitted on disk");
    check(content.find("### What did not work") == std::string::npos,
          "F: did_not_work heading omitted on disk");
    check(content.find("### Open questions") == std::string::npos,
          "F: open_questions heading omitted on disk");
    check(content.find("### Decisions") == std::string::npos,
          "F: decisions heading omitted on disk");
}

// ---- Case G: parser delimits LEARNINGS_UPDATE in markdown context ----------
// Real scribe output wraps the block in narrative text. The parser must
// pick the block out of the surrounding free_text, leaving text before
// and after as free_text rather than swallowing it.
static void test_G_parser_block_in_context(const fs::path& tdir) {
    std::cout << "\n=== Case G: parser delimits LEARNINGS_UPDATE in markdown ===\n\n";

    const std::string raw =
        "Some narrative summary before the block.\n"
        "Another line of free text.\n"
        "\n"
        "```LEARNINGS_UPDATE\n"
        "utc: 2026-05-29T03:15:30Z\n"
        "tried: |\n"
        "  - tried X in context\n"
        "decisions: |\n"
        "  - decided Y in context\n"
        "```\n"
        "\n"
        "And a trailing line of free text after the block.\n";

    sui::quorum::OutputParser parser;
    auto parsed = parser.parse(raw);

    check(parsed.learnings_updates.size() == 1,
          "G: exactly one learnings_updates entry parsed");
    if (parsed.learnings_updates.size() != 1) return;

    const auto& e = parsed.learnings_updates[0];
    check(e.utc_timestamp == "2026-05-29T03:15:30Z", "G: utc parsed in context");
    check(e.tried.size() == 1 && e.tried[0] == "tried X in context",
          "G: tried bullet parsed in context");
    check(e.decisions.size() == 1 && e.decisions[0] == "decided Y in context",
          "G: decisions bullet parsed in context");
    check(e.worked.empty() && e.did_not_work.empty() && e.open_questions.empty(),
          "G: unmentioned sub-fields empty");

    // free_text should contain the narrative around the block but NOT
    // the block's structured field lines.
    check(parsed.free_text.find("Some narrative summary before the block.")
              != std::string::npos,
          "G: pre-block free_text preserved");
    check(parsed.free_text.find("trailing line of free text after the block.")
              != std::string::npos,
          "G: post-block free_text preserved");
    check(parsed.free_text.find("utc: 2026-05-29T03:15:30Z") == std::string::npos,
          "G: block sub-fields not leaked into free_text");

    // End-to-end on-disk check.
    auto proj = tdir / "G";
    fs::create_directories(proj);
    auto r = sui::quorum::apply_scribe_learnings_update(proj.string(), e);
    check(r.ok, "G: apply_scribe_learnings_update ok");

    auto content = read_file(proj / ".quorum" / "learnings.md");
    check(content.find("## Learnings, 2026-05-29T03:15:30Z") != std::string::npos,
          "G: session heading on disk");
    check(content.find("- tried X in context") != std::string::npos,
          "G: tried bullet on disk");
    check(content.find("- decided Y in context") != std::string::npos,
          "G: decisions bullet on disk");
}

// ---- main ------------------------------------------------------------------
int main() {
    auto tdir = fs::temp_directory_path() /
                ("quorum-test-scribe-write-discipline-" +
                 std::to_string(::getpid()));
    fs::create_directories(tdir);

    test_A_bootstrap(tdir);
    test_B_append_only(tdir);
    test_C_timestamps(tdir);
    test_D_validate_headers();
    test_E_parser_basic(tdir);
    test_F_parser_empty_subfields_omitted(tdir);
    test_G_parser_block_in_context(tdir);

    std::error_code ec;
    fs::remove_all(tdir, ec);

    std::cout << "\nResults: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
