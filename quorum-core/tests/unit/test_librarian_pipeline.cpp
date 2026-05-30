// tests/unit/test_librarian_pipeline.cpp
// Phase 11 Batch B - librarian curate PIPELINE (parse -> validate -> diff ->
// apply). Drives run_curation_pipeline with a CANNED raw librarian output
// string against a seeded skeleton project. NO live claude call.
//
// Assertions (pitch-protocol.md v0.1 / Batch B contract):
//   (A) PreviewOnly: no files changed; plan lists 3 proposals with diffs.
//   (B) ApplyAll: the 2 sections replaced + 1 decision appended; file contents
//       assert the writes landed.
//   (C) Invalid section: that proposal status=rejected; the valid ones still
//       apply.
//
// Run:  cd build && ctest -R test_librarian_pipeline --output-on-failure

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <unistd.h>

#include "cli/librarian_curate.h"

namespace fs = std::filesystem;
using namespace sui::quorum;
using sui::quorum::cli::ApplyMode;
using sui::quorum::cli::run_curation_pipeline;

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

// Canned librarian output: 2 CURATION_UPDATE (one to Current direction, one to
// Anti-goals) + 1 DECISION_LOG_APPEND. Mirrors the pitch-protocol worked
// example shape (explicit fences).
static const char* kCannedOutput = R"OUT(Here is my curation pass.

```CURATION_UPDATE
file: Pitch/00 - Introduction.md
section: Current direction
content: |
  - Section-scoped diff gate preserves operator edits across re-runs.
source: learnings.md 2026-05-27T10:00:00Z
```

```CURATION_UPDATE
file: Pitch/01 - Anti-goals.md
section: Anti-goals
content: |
  - Do NOT grant the librarian executor tools. (2026-05-28)
source: learnings.md 2026-05-28T14:00:00Z
```

```DECISION_LOG_APPEND
utc: 2026-05-29T09:00:00Z
decision: |
  Manual CLI trigger only for v0.1.
rationale: |
  Matches the manual-gate posture of Phase 10 hygiene CLIs.
source: learnings.md 2026-05-29T09:00:00Z
```
)OUT";

// Canned output where one CURATION_UPDATE targets an invalid section. The two
// other blocks are valid (Anti-goals update + decision append).
static const char* kCannedWithBad = R"OUT(```CURATION_UPDATE
file: Pitch/00 - Introduction.md
section: Not A Real Section
content: |
  - this should be rejected
source: learnings.md 2026-05-27T10:00:00Z
```

```CURATION_UPDATE
file: Pitch/01 - Anti-goals.md
section: Anti-goals
content: |
  - Valid anti-goal that must still apply.
source: learnings.md 2026-05-28T14:00:00Z
```

```DECISION_LOG_APPEND
utc: 2026-05-29T09:00:00Z
decision: |
  A valid decision that must still append.
source: learnings.md 2026-05-29T09:00:00Z
```
)OUT";

// ---- Case A: PreviewOnly — no writes, 3 proposals, diffs populated ----------
static void test_A_preview(const fs::path& tdir) {
    std::cout << "\n=== Case A: PreviewOnly (no writes) ===\n\n";
    auto proj = tdir / "A";
    fs::create_directories(proj);
    auto seed = ensure_curation_skeleton(proj.string());
    check(seed.ok, "A: skeleton seeded");

    // Curated layer lives under <proj>/.quorum/librarian/ (curated_base).
    auto curated   = proj / ".quorum" / "librarian";
    auto intro     = curated / "Pitch" / "00 - Introduction.md";
    auto antigoals = curated / "Pitch" / "01 - Anti-goals.md";
    auto declog    = curated / "00 - Decision Log.md";

    auto before_intro     = read_file(intro);
    auto before_antigoals = read_file(antigoals);
    auto before_declog    = read_file(declog);

    auto plan = run_curation_pipeline(proj.string(), kCannedOutput,
                                      ApplyMode::PreviewOnly);

    check(plan.proposals.size() == 3, "A: 3 proposals parsed");

    // Files byte-identical (PreviewOnly never writes).
    check(read_file(intro) == before_intro, "A: intro unchanged");
    check(read_file(antigoals) == before_antigoals, "A: anti-goals unchanged");
    check(read_file(declog) == before_declog, "A: decision log unchanged");

    // All proposals previewed with a non-empty diff.
    bool all_previewed = true, all_have_diff = true;
    for (const auto& p : plan.proposals) {
        if (p.status != "previewed") all_previewed = false;
        if (p.diff.empty()) all_have_diff = false;
    }
    check(all_previewed, "A: every proposal status == previewed");
    check(all_have_diff, "A: every proposal has a diff");
    check(plan.applied_count() == 0, "A: applied_count == 0");
}

// ---- Case B: ApplyAll — 2 sections replaced + 1 decision appended -----------
static void test_B_apply_all(const fs::path& tdir) {
    std::cout << "\n=== Case B: ApplyAll (writes land) ===\n\n";
    auto proj = tdir / "B";
    fs::create_directories(proj);
    auto seed = ensure_curation_skeleton(proj.string());
    check(seed.ok, "B: skeleton seeded");

    auto curated   = proj / ".quorum" / "librarian";
    auto intro     = curated / "Pitch" / "00 - Introduction.md";
    auto antigoals = curated / "Pitch" / "01 - Anti-goals.md";
    auto declog    = curated / "00 - Decision Log.md";

    auto plan = run_curation_pipeline(proj.string(), kCannedOutput,
                                      ApplyMode::ApplyAll);

    check(plan.proposals.size() == 3, "B: 3 proposals parsed");
    check(plan.applied_count() == 3, "B: all 3 applied");
    check(plan.rejected_count() == 0, "B: none rejected");

    auto intro_c = read_file(intro);
    check(intro_c.find("Section-scoped diff gate preserves operator edits "
                       "across re-runs.") != std::string::npos,
          "B: Current direction body written to intro");
    // Other intro sections preserved.
    check(intro_c.find("## What we're building") != std::string::npos,
          "B: intro 'What we're building' preserved");
    check(intro_c.find("See [[01 - Anti-goals]].") != std::string::npos,
          "B: intro trailing wikilink preserved");

    auto antigoals_c = read_file(antigoals);
    check(antigoals_c.find("Do NOT grant the librarian executor tools.")
              != std::string::npos,
          "B: Anti-goals body written");

    auto declog_c = read_file(declog);
    check(declog_c.find("### 2026-05-29 \xe2\x80\x94 Manual CLI trigger only "
                        "for v0.1.") != std::string::npos,
          "B: decision appended with canonical heading");
    check(declog_c.find("**Why:** Matches the manual-gate posture")
              != std::string::npos,
          "B: decision **Why:** line present");
    check(declog_c.find("**Source:** learnings.md 2026-05-29T09:00:00Z")
              != std::string::npos,
          "B: decision **Source:** line present");
}

// ---- Case C: invalid section rejected, others still apply -------------------
static void test_C_invalid_section(const fs::path& tdir) {
    std::cout << "\n=== Case C: invalid section rejected, others apply ===\n\n";
    auto proj = tdir / "C";
    fs::create_directories(proj);
    auto seed = ensure_curation_skeleton(proj.string());
    check(seed.ok, "C: skeleton seeded");

    auto curated   = proj / ".quorum" / "librarian";
    auto intro     = curated / "Pitch" / "00 - Introduction.md";
    auto antigoals = curated / "Pitch" / "01 - Anti-goals.md";
    auto declog    = curated / "00 - Decision Log.md";

    auto plan = run_curation_pipeline(proj.string(), kCannedWithBad,
                                      ApplyMode::ApplyAll);

    check(plan.proposals.size() == 3, "C: 3 proposals parsed");
    check(plan.rejected_count() == 1, "C: exactly 1 rejected");
    check(plan.applied_count() == 2, "C: 2 still applied");

    // Find the rejected proposal and confirm it's the bad-section one.
    bool found_bad = false;
    for (const auto& p : plan.proposals) {
        if (p.status == "rejected") {
            found_bad = true;
            check(p.target.find("Not A Real Section") != std::string::npos,
                  "C: rejected proposal is the invalid-section one");
            check(!p.reason.empty(), "C: rejected proposal has a reason");
        }
    }
    check(found_bad, "C: a rejected proposal exists");

    // The invalid section must NOT have leaked into intro.
    auto intro_c = read_file(intro);
    check(intro_c.find("## Not A Real Section") == std::string::npos,
          "C: invalid section heading not written to intro");
    check(intro_c.find("this should be rejected") == std::string::npos,
          "C: rejected content not written");

    // The two valid proposals landed.
    auto antigoals_c = read_file(antigoals);
    check(antigoals_c.find("Valid anti-goal that must still apply.")
              != std::string::npos,
          "C: valid Anti-goals update applied");
    auto declog_c = read_file(declog);
    check(declog_c.find("A valid decision that must still append.")
              != std::string::npos,
          "C: valid decision append applied");
}

// ---- main ------------------------------------------------------------------
int main() {
    auto tdir = fs::temp_directory_path() /
                ("quorum-test-librarian-pipeline-" + std::to_string(::getpid()));
    fs::create_directories(tdir);

    test_A_preview(tdir);
    test_B_apply_all(tdir);
    test_C_invalid_section(tdir);

    std::error_code ec;
    fs::remove_all(tdir, ec);

    std::cout << "\nResults: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
