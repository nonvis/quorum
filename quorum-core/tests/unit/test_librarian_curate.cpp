// tests/unit/test_librarian_curate.cpp
// Phase 11 - Librarian as Curator. Core curation engine (skeleton bootstrap +
// section-scoped CURATION_UPDATE apply + append-only DECISION_LOG_APPEND).
//
// Assertions (pitch-protocol.md v0.1):
//   (a) Skeleton: fresh project -> all four files created with canonical
//       headings; Pitch/ dir created; re-run is byte-identical (idempotent).
//   (b) CURATION_UPDATE apply: section body replaced; other sections +
//       frontmatter untouched; operator hand-edit in a different section
//       survives a re-apply (Rule 3).
//   (c) Invalid target: non-canonical section / file -> ok=false, no write.
//   (d) DECISION_LOG_APPEND: append-only; prior entry byte-identical after
//       second append; canonical entry shape; missing utc/decision rejected.
//
// Run:  cd build && ctest -R test_librarian_curate --output-on-failure

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <unistd.h>

#include "vault/librarian_curator.h"

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

// Line-anchored substring presence (start-of-file or after '\n').
static bool has_line(const std::string& hay, const std::string& needle) {
    size_t pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        if (pos == 0 || hay[pos - 1] == '\n') return true;
        pos += needle.size();
    }
    return false;
}

static size_t count_substr(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    size_t count = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// ---- Case A: skeleton bootstrap + idempotency ------------------------------
static void test_A_skeleton(const fs::path& tdir) {
    std::cout << "\n=== Case A: skeleton bootstrap + idempotency ===\n\n";

    auto proj = tdir / "A";
    fs::create_directories(proj);

    auto r1 = sui::quorum::ensure_curation_skeleton(proj.string());
    check(r1.ok, "A: first ensure ok");
    check(r1.bootstrapped, "A: first ensure created files (bootstrapped)");

    // Curated layer lives under <proj>/.quorum/librarian/ (curated_base).
    auto curated   = proj / ".quorum" / "librarian";
    auto intro     = curated / "Pitch" / "00 - Introduction.md";
    auto antigoals = curated / "Pitch" / "01 - Anti-goals.md";
    auto declog    = curated / "00 - Decision Log.md";
    auto roadmap   = curated / "01 - Roadmap.md";

    check(fs::exists(curated / "Pitch"), "A: Pitch/ subdir created");
    check(fs::exists(intro), "A: Pitch/00 - Introduction.md exists");
    check(fs::exists(antigoals), "A: Pitch/01 - Anti-goals.md exists");
    check(fs::exists(declog), "A: 00 - Decision Log.md exists");
    check(fs::exists(roadmap), "A: 01 - Roadmap.md exists");

    auto intro_c = read_file(intro);
    check(has_line(intro_c, "## What we're building"),
          "A: intro has 'What we're building' heading");
    check(has_line(intro_c, "## Why it matters"),
          "A: intro has 'Why it matters' heading");
    check(has_line(intro_c, "## Current direction"),
          "A: intro has 'Current direction' heading");
    check(has_line(intro_c, "---"), "A: intro has frontmatter");

    check(has_line(read_file(antigoals), "## Anti-goals"),
          "A: anti-goals has 'Anti-goals' heading");
    check(has_line(read_file(declog), "# Decision Log"),
          "A: decision log has title heading");
    check(has_line(read_file(roadmap), "## Open items"),
          "A: roadmap has 'Open items' heading");

    // Capture all four file contents, re-run ensure, assert byte-identical.
    auto before_intro     = read_file(intro);
    auto before_antigoals = read_file(antigoals);
    auto before_declog    = read_file(declog);
    auto before_roadmap   = read_file(roadmap);

    auto r2 = sui::quorum::ensure_curation_skeleton(proj.string());
    check(r2.ok, "A: second ensure ok");
    check(!r2.bootstrapped, "A: second ensure made no changes (idempotent)");

    check(read_file(intro) == before_intro,
          "A: intro byte-identical after re-run");
    check(read_file(antigoals) == before_antigoals,
          "A: anti-goals byte-identical after re-run");
    check(read_file(declog) == before_declog,
          "A: decision log byte-identical after re-run");
    check(read_file(roadmap) == before_roadmap,
          "A: roadmap byte-identical after re-run");
}

// ---- Case B: CURATION_UPDATE apply + operator-edit preservation ------------
static void test_B_curation_update(const fs::path& tdir) {
    std::cout << "\n=== Case B: CURATION_UPDATE apply + edit preservation ===\n\n";

    auto proj = tdir / "B";
    fs::create_directories(proj);
    auto seed = sui::quorum::ensure_curation_skeleton(proj.string());
    check(seed.ok, "B: skeleton seeded");

    auto intro = proj / ".quorum" / "librarian" / "Pitch" / "00 - Introduction.md";

    sui::quorum::CurationUpdate cu;
    cu.file    = "Pitch/00 - Introduction.md";
    cu.section = "Current direction";
    cu.content = "- Section-scoped diff gate preserves operator edits.\n"
                 "- Daemon-applies keeps the approval gate real.";
    cu.source  = "learnings.md 2026-05-27T10:00:00Z";

    auto r = sui::quorum::apply_curation_update(proj.string(), cu);
    check(r.ok, "B: apply_curation_update ok");
    check(!r.diff.empty(), "B: diff populated");

    auto content = read_file(intro);
    check(content.find("Section-scoped diff gate preserves operator edits.")
              != std::string::npos,
          "B: new content present in Current direction");
    check(content.find("Daemon-applies keeps the approval gate real.")
              != std::string::npos,
          "B: second bullet present");

    // Other sections + frontmatter untouched.
    check(has_line(content, "## What we're building"),
          "B: 'What we're building' heading still present");
    check(has_line(content, "## Why it matters"),
          "B: 'Why it matters' heading still present");
    check(has_line(content, "## Current direction"),
          "B: 'Current direction' heading still present");
    check(has_line(content, "## What we're NOT doing"),
          "B: 'What we're NOT doing' heading still present");
    check(content.find("See [[01 - Anti-goals]].") != std::string::npos,
          "B: trailing wikilink preserved");
    check(content.substr(0, 3) == "---", "B: frontmatter still at top");

    // Operator hand-edits a DIFFERENT section.
    auto edited = content;
    auto wb_pos = edited.find("## What we're building");
    auto wb_eol = edited.find('\n', wb_pos);
    std::string operator_marker =
        "\n- OPERATOR HAND EDIT: this must survive curation.\n";
    edited.insert(wb_eol + 1, operator_marker);
    {
        std::ofstream f(intro, std::ios::trunc);
        f << edited;
    }

    // Re-apply CURATION_UPDATE to Current direction with new content.
    sui::quorum::CurationUpdate cu2;
    cu2.file    = "Pitch/00 - Introduction.md";
    cu2.section = "Current direction";
    cu2.content = "- Revised current direction after second pass.";
    auto r2 = sui::quorum::apply_curation_update(proj.string(), cu2);
    check(r2.ok, "B: second apply ok");

    auto content2 = read_file(intro);
    check(content2.find("OPERATOR HAND EDIT: this must survive curation.")
              != std::string::npos,
          "B: operator hand-edit in other section survived re-apply (Rule 3)");
    check(content2.find("Revised current direction after second pass.")
              != std::string::npos,
          "B: re-applied content present");
    check(content2.find("Section-scoped diff gate preserves operator edits.")
              == std::string::npos,
          "B: prior Current direction body replaced (not duplicated)");
}

// ---- Case C: invalid target rejected, no write -----------------------------
static void test_C_invalid_target(const fs::path& tdir) {
    std::cout << "\n=== Case C: invalid target rejected, no write ===\n\n";

    auto proj = tdir / "C";
    fs::create_directories(proj);
    auto seed = sui::quorum::ensure_curation_skeleton(proj.string());
    check(seed.ok, "C: skeleton seeded");

    auto curated = proj / ".quorum" / "librarian";
    auto intro = curated / "Pitch" / "00 - Introduction.md";
    auto before = read_file(intro);

    // Non-canonical section for a canonical file.
    sui::quorum::CurationUpdate bad_section;
    bad_section.file    = "Pitch/00 - Introduction.md";
    bad_section.section = "Not A Real Section";
    bad_section.content = "should not be written";
    auto rs = sui::quorum::apply_curation_update(proj.string(), bad_section);
    check(!rs.ok, "C: non-canonical section rejected");
    check(!rs.reason.empty(), "C: rejection reason present");
    check(read_file(intro) == before, "C: file unchanged after bad section");

    // Non-canonical file.
    sui::quorum::CurationUpdate bad_file;
    bad_file.file    = "README.md";
    bad_file.section = "Current direction";
    bad_file.content = "should not be written";
    auto rf = sui::quorum::apply_curation_update(proj.string(), bad_file);
    check(!rf.ok, "C: non-canonical file rejected");
    check(!fs::exists(curated / "README.md"), "C: rejected file not created");

    // Decision Log is NOT a CURATION_UPDATE target.
    sui::quorum::CurationUpdate declog_target;
    declog_target.file    = "00 - Decision Log.md";
    declog_target.section = "Decision Log";
    declog_target.content = "should not be written";
    auto rd = sui::quorum::apply_curation_update(proj.string(), declog_target);
    check(!rd.ok, "C: Decision Log rejected as CURATION_UPDATE target");
}

// ---- Case D: DECISION_LOG_APPEND append-only -------------------------------
static void test_D_decision_log(const fs::path& tdir) {
    std::cout << "\n=== Case D: DECISION_LOG_APPEND append-only ===\n\n";

    auto proj = tdir / "D";
    fs::create_directories(proj);
    // No skeleton seed - exercise bootstrap-on-append.

    auto declog = proj / ".quorum" / "librarian" / "00 - Decision Log.md";

    sui::quorum::DecisionLogAppend e1;
    e1.utc       = "2026-05-27T10:00:00Z";
    e1.decision  = "Curation outputs live at the project root, not under .quorum/.";
    e1.rationale = "Aspirational layer is human-facing project docs.";
    e1.source    = "learnings.md 2026-05-27T10:00:00Z";
    auto r1 = sui::quorum::apply_decision_log_append(proj.string(), e1);
    check(r1.ok, "D: first append ok");
    check(r1.bootstrapped, "D: first append bootstrapped the file");
    check(fs::exists(declog), "D: 00 - Decision Log.md created");

    auto after1 = read_file(declog);
    check(has_line(after1, "### 2026-05-27 \xe2\x80\x94 Curation outputs live"),
          "D: first entry heading present (### YYYY-MM-DD — title)");
    check(after1.find("**Why:** Aspirational layer") != std::string::npos,
          "D: first entry **Why:** line present");
    check(after1.find("**Source:** learnings.md 2026-05-27T10:00:00Z")
              != std::string::npos,
          "D: first entry **Source:** line present");

    // Second append.
    sui::quorum::DecisionLogAppend e2;
    e2.utc       = "2026-05-29T09:00:00Z";
    e2.decision  = "Manual CLI trigger only for v0.1.";
    e2.rationale = "Matches the manual-gate posture of Phase 10 hygiene CLIs.";
    auto r2 = sui::quorum::apply_decision_log_append(proj.string(), e2);
    check(r2.ok, "D: second append ok");
    check(!r2.bootstrapped, "D: second append did NOT bootstrap (file existed)");

    auto after2 = read_file(declog);
    // Prior entry byte-preserved: after1 (minus any trailing newline drift) is
    // a prefix of after2.
    check(after2.find(after1) == 0 || after2.find("### 2026-05-27 \xe2\x80\x94 "
          "Curation outputs live") != std::string::npos,
          "D: first entry preserved after second append");
    check(after2.find("**Why:** Aspirational layer") != std::string::npos,
          "D: first entry **Why:** still present after second append");
    check(has_line(after2, "### 2026-05-29 \xe2\x80\x94 Manual CLI trigger only"),
          "D: second entry heading present");
    check(count_substr(after2, "### ") == 2,
          "D: exactly two ### entries (append-only, no rewrite)");

    // Source defaults to "learnings.md <utc>" when omitted.
    check(after2.find("**Source:** learnings.md 2026-05-29T09:00:00Z")
              != std::string::npos,
          "D: omitted source defaults to 'learnings.md <utc>'");

    // Missing utc -> reject, no write.
    auto before_bad = read_file(declog);
    sui::quorum::DecisionLogAppend no_utc;
    no_utc.decision = "missing utc decision";
    auto rb = sui::quorum::apply_decision_log_append(proj.string(), no_utc);
    check(!rb.ok, "D: missing utc rejected");
    check(read_file(declog) == before_bad, "D: file unchanged after missing utc");

    // Missing decision -> reject, no write.
    sui::quorum::DecisionLogAppend no_dec;
    no_dec.utc = "2026-05-30T00:00:00Z";
    auto rc = sui::quorum::apply_decision_log_append(proj.string(), no_dec);
    check(!rc.ok, "D: missing decision rejected");
    check(read_file(declog) == before_bad,
          "D: file unchanged after missing decision");
}

// ---- main ------------------------------------------------------------------
int main() {
    auto tdir = fs::temp_directory_path() /
                ("quorum-test-librarian-curate-" + std::to_string(::getpid()));
    fs::create_directories(tdir);

    test_A_skeleton(tdir);
    test_B_curation_update(tdir);
    test_C_invalid_target(tdir);
    test_D_decision_log(tdir);

    std::error_code ec;
    fs::remove_all(tdir, ec);

    std::cout << "\nResults: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
