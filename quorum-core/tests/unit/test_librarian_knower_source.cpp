// tests/unit/test_librarian_knower_source.cpp
// Phase 11 (pitch-protocol v0.4) — knower-vaults-as-source for the librarian
// Pitch/Roadmap lanes. The curation source of truth is linearized:
//   scribe journal -> knower vaults (authoritative, by lens) -> librarian view.
//
// Focus: detail::knower_vault_digest(project_root) — the digest that feeds the
// Pitch/Roadmap lanes. Assertions:
//   (a) enumerates ref-*.md across MULTIPLE knower vaults (cartographer +
//       architect + historian) and project-promoted .quorum/knowledge/ref-*.md;
//   (b) EXCLUDES rule-*.md and the scribe vault from the Pitch/Roadmap source;
//   (c) is BOUNDED (filename list for all; head/summary excerpt for at most
//       max_recent; head_chars cap honored; summary: preferred when present);
//   (d) fallback: with NO known-knower names present, falls back to
//       all-vaults-except-scribe;
//   (e) idempotency INTENT: an unchanged knower vault yields no Pitch/Roadmap
//       proposals — modeled as the librarian emitting empty output (the correct
//       no-delta response) producing zero proposals through the pipeline.
//
// Run:  cd build && ctest -R test_librarian_knower_source --output-on-failure

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "cli/librarian_curate.h"

namespace fs = std::filesystem;

static int g_passed = 0;
static int g_failed = 0;

#define check(cond, msg) do {                                         \
    if (cond) { ++g_passed; std::cout << "  PASS: " << msg << "\n"; } \
    else      { ++g_failed; std::cerr << "  FAIL: " << msg << "\n"; } \
} while (0)

static void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::trunc);
    f << body;
}

// Seed a knower vault's knowledge dir with one ref-*.md (+ optional summary).
static void seed_ref(const fs::path& vaults_root, const std::string& knower,
                     const std::string& slug, const std::string& summary,
                     const std::string& body) {
    auto p = vaults_root / knower / "knowledge" / ("ref-" + slug + ".md");
    std::string content;
    content += "---\n";
    content += "tags: [test]\n";
    if (!summary.empty()) content += "summary: " + summary + "\n";
    content += "---\n";
    content += body;
    write_file(p, content);
}

// ---- Case A: enumerate across multiple knower vaults + project refs ----------
static void test_A_enumerate(const fs::path& tdir) {
    std::cout << "\n=== Case A: enumerate ref-* across knower vaults ===\n\n";
    auto proj = tdir / "A";
    auto vaults = proj / ".quorum" / "vaults";

    seed_ref(vaults, "cartographer", "project-index",
             "Where is X — top-level component map.",
             "# Project Index\n\nComponents...\n");
    seed_ref(vaults, "architect", "design",
             "Why the design is shaped this way.",
             "# Design\n\nLayering decisions...\n");
    seed_ref(vaults, "historian", "timeline", "",
             "# Timeline\n\nEvolution of the project...\n");
    // Project-scope promoted ref (vault_manager auto-promotion target).
    write_file(proj / ".quorum" / "knowledge" / "ref-promoted.md",
               "---\nsummary: Promoted cross-cutting ref.\n---\n# Promoted\n");

    auto digest = sui::quorum::cli::detail::knower_vault_digest(proj);

    check(digest.find("ref-project-index.md") != std::string::npos,
          "A: cartographer ref enumerated");
    check(digest.find("ref-design.md") != std::string::npos,
          "A: architect ref enumerated");
    check(digest.find("ref-timeline.md") != std::string::npos,
          "A: historian ref enumerated");
    check(digest.find("ref-promoted.md") != std::string::npos,
          "A: project-promoted ref enumerated");
    check(digest.find("[vault: cartographer]") != std::string::npos,
          "A: cartographer source label present");
    check(digest.find("[vault: architect]") != std::string::npos,
          "A: architect source label present");
    check(digest.find("[project knowledge]") != std::string::npos,
          "A: project-knowledge source label present");

    // summary: preferred over raw head when present.
    check(digest.find("summary: Why the design is shaped this way.")
              != std::string::npos,
          "A: frontmatter summary: preferred as excerpt");
}

// ---- Case B: exclude rule-* and the scribe vault ----------------------------
static void test_B_exclusions(const fs::path& tdir) {
    std::cout << "\n=== Case B: exclude rule-* and scribe vault ===\n\n";
    auto proj = tdir / "B";
    auto vaults = proj / ".quorum" / "vaults";

    seed_ref(vaults, "architect", "keep", "Authoritative architecture ref.",
             "# Keep\n");
    // A rule-*.md in the same knower vault — must NOT appear in the digest.
    write_file(vaults / "architect" / "knowledge" / "rule-style.md",
               "---\nsummary: A rule, not pitch material.\n---\n# Rule\n");
    // The scribe vault — must NOT contribute to the Pitch/Roadmap source.
    seed_ref(vaults, "scribe", "scribe-note", "Scribe journal note.",
             "# Scribe note\n");

    auto digest = sui::quorum::cli::detail::knower_vault_digest(proj);

    check(digest.find("ref-keep.md") != std::string::npos,
          "B: architect ref-* included");
    check(digest.find("rule-style.md") == std::string::npos,
          "B: rule-*.md EXCLUDED from Pitch/Roadmap source");
    check(digest.find("A rule, not pitch material.") == std::string::npos,
          "B: rule-*.md summary not leaked");
    check(digest.find("ref-scribe-note.md") == std::string::npos,
          "B: scribe vault ref EXCLUDED from Pitch/Roadmap source");
    check(digest.find("[vault: scribe]") == std::string::npos,
          "B: scribe vault not labelled as a knower source");
}

// ---- Case C: bounded output -------------------------------------------------
static void test_C_bounded(const fs::path& tdir) {
    std::cout << "\n=== Case C: digest is bounded ===\n\n";
    auto proj = tdir / "C";
    auto vaults = proj / ".quorum" / "vaults";

    // Seed 8 refs across knowers; only max_recent get a head/summary excerpt,
    // but all 8 appear in the filename list.
    for (int i = 0; i < 8; ++i) {
        std::string slug = "n" + std::to_string(i);
        seed_ref(vaults, "cartographer", slug, "", "# Ref " + slug + "\n");
    }
    // A long ref with NO summary to exercise the head_chars cap.
    std::string huge(5000, 'x');
    seed_ref(vaults, "architect", "huge", "",
             "# Huge\n\n" + huge + "\n");

    const size_t max_recent = 5;
    const size_t head_chars = 800;
    auto digest = sui::quorum::cli::detail::knower_vault_digest(
        proj, max_recent, head_chars);

    // All 9 refs listed in the filename block (count "- ref-" line prefixes in
    // the listing section is fiddly; assert the count of "ref-" occurrences in
    // the excerpt section is capped instead).
    size_t excerpt_headers = 0;
    {
        size_t pos = 0;
        const std::string marker = "#### ref-";
        while ((pos = digest.find(marker, pos)) != std::string::npos) {
            ++excerpt_headers;
            pos += marker.size();
        }
    }
    check(excerpt_headers == max_recent,
          "C: at most max_recent (5) refs get an excerpt header");

    // The huge ref, if shown as an excerpt, is truncated (head_chars cap).
    // It is the most-recently-written, so it is shown. Confirm truncation marker
    // present and the full 5000-char blob is NOT inlined.
    check(digest.find("... (truncated)") != std::string::npos,
          "C: long ref excerpt truncated (head_chars cap honored)");
    check(digest.find(huge) == std::string::npos,
          "C: full long-ref body NOT inlined (bounded)");
}

// ---- Case D: fallback to all-but-scribe when no known knower names ----------
static void test_D_fallback(const fs::path& tdir) {
    std::cout << "\n=== Case D: fallback all-but-scribe ===\n\n";
    auto proj = tdir / "D";
    auto vaults = proj / ".quorum" / "vaults";

    // No cartographer/architect/historian/recap — only a custom knower + scribe.
    seed_ref(vaults, "domain-expert", "insight", "Custom knower insight.",
             "# Insight\n");
    seed_ref(vaults, "scribe", "journal", "Scribe note.", "# Journal\n");

    auto digest = sui::quorum::cli::detail::knower_vault_digest(proj);

    check(digest.find("ref-insight.md") != std::string::npos,
          "D: custom (non-standard) knower ref included via fallback");
    check(digest.find("[vault: domain-expert]") != std::string::npos,
          "D: custom knower labelled");
    check(digest.find("ref-journal.md") == std::string::npos,
          "D: scribe still excluded under fallback");
}

// ---- Case E: empty source -> sentinel, and idempotency intent ---------------
static void test_E_empty_and_idempotent(const fs::path& tdir) {
    std::cout << "\n=== Case E: empty source + idempotency intent ===\n\n";

    // No knower vaults at all -> sentinel (no authoritative source yet).
    auto empty_proj = tdir / "E_empty";
    fs::create_directories(empty_proj / ".quorum");
    auto empty_digest = sui::quorum::cli::detail::knower_vault_digest(empty_proj);
    check(empty_digest.find("no knower-vault ref-*.md notes found")
              != std::string::npos,
          "E: empty knower source reports sentinel");

    // Idempotency INTENT: when the knower vaults are unchanged, the librarian's
    // correct response is to emit no new Pitch/Roadmap deltas. Model that as an
    // empty librarian output and assert the pipeline yields zero proposals
    // (no Pitch/Roadmap CURATION_UPDATE invented from nothing).
    auto proj = tdir / "E_idem";
    fs::create_directories(proj);
    auto seed = sui::quorum::ensure_curation_skeleton(proj.string());
    check(seed.ok, "E: skeleton seeded");

    auto plan = sui::quorum::cli::run_curation_pipeline(
        proj.string(), "No new knower-vault content; nothing to curate.",
        sui::quorum::cli::ApplyMode::PreviewOnly);
    check(plan.proposals.empty(),
          "E: unchanged knower vaults -> no Pitch/Roadmap proposals");
}

// ---- main -------------------------------------------------------------------
int main() {
    auto tdir = fs::temp_directory_path() /
                ("quorum-test-librarian-knower-source-" +
                 std::to_string(::getpid()));
    fs::create_directories(tdir);

    test_A_enumerate(tdir);
    test_B_exclusions(tdir);
    test_C_bounded(tdir);
    test_D_fallback(tdir);
    test_E_empty_and_idempotent(tdir);

    std::error_code ec;
    fs::remove_all(tdir, ec);

    std::cout << "\nResults: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
