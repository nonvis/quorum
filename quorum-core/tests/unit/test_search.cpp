// tests/unit/test_search.cpp
// Phase 15 — `quorum search` deterministic no-LLM ranked ref search.
//
// Exercises the PURE search_refs() entrypoint (cli/search.h) directly — no
// stdout, no claude, no daemon. Builds a temp .quorum/vaults/<knower>/knowledge/
// fixture mirroring the real layout and asserts:
//   S1. a query matching a frontmatter TAG outranks one matching only body
//       text (tag ×5 vs content ×1).
//   S2. the preview is the `summary:` field when present, the body excerpt
//       when absent.
//   S3. --agent scoping returns only that vault's refs (other vaults + the
//       project-scope refs are excluded).
//   S4. an empty / stopword-only query yields no hits without crashing.
//   S5. corpus spans EVERY vault subdir with a knowledge/ dir (custom/doer
//       vaults too) plus project-scope .quorum/knowledge/.
//
// Run: ctest -R test_search --output-on-failure

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <unistd.h>

#include "cli/search.h"

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

// Build a temp project root with a .quorum/ dir. Returns the project root path.
static std::string make_temp_project() {
    auto dir = fs::temp_directory_path() /
        ("quorum_test_search_" + std::to_string(getpid()) + "_" +
         std::to_string(g_test_num++));
    fs::create_directories(dir / ".quorum");
    return dir.string();
}

static void cleanup(const std::string& path) {
    fs::remove_all(path);
}

// Write a ref-*.md into <root>/.quorum/vaults/<vault>/knowledge/, creating the
// dir tree. The 15ms sleep separates mtimes so the DESC tie-break is stable.
static void write_vault_ref(const std::string& root, const std::string& vault,
                            const std::string& name,
                            const std::string& content) {
    auto kd = fs::path(root) / ".quorum" / "vaults" / vault / "knowledge";
    fs::create_directories(kd);
    std::ofstream out(kd / name);
    out << content;
    out.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
}

// Write a ref-*.md into the project-scope <root>/.quorum/knowledge/ dir.
static void write_project_ref(const std::string& root, const std::string& name,
                              const std::string& content) {
    auto kd = fs::path(root) / ".quorum" / "knowledge";
    fs::create_directories(kd);
    std::ofstream out(kd / name);
    out << content;
    out.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
}

using sui::quorum::cli::SearchOptions;
using sui::quorum::cli::SearchHit;
using sui::quorum::cli::search_refs;

static const SearchHit* find_hit(const std::vector<SearchHit>& hits,
                                 const std::string& filename) {
    for (const auto& h : hits) {
        if (h.path.filename().string() == filename) return &h;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// S1: tag-match (×5) outranks body-only match (×1)
// ---------------------------------------------------------------------------
static void test_s1_tag_outranks_body() {
    std::cout << "\n=== S1. tag-match (×5) outranks body-only (×1) ===\n\n";
    auto root = make_temp_project();

    // ref-tagged.md: query word "walrus" in a frontmatter tag only.
    //   tag_hits=1 (×5), content_hits=0 → 5
    write_vault_ref(root, "move-dev", "ref-tagged.md",
        "---\ntags: [walrus]\n---\n# Tagged\n\nirrelevant body content here.\n");
    // ref-body.md: query word "walrus" in the body only, untagged, not in fn.
    //   content_hits=1 → 1
    write_vault_ref(root, "move-dev", "ref-body.md",
        "# Untagged\n\nThis body mentions walrus exactly once.\n");

    SearchOptions opts;
    opts.query = "walrus";
    opts.limit = 10;
    size_t nrefs = 0, nvaults = 0;
    auto hits = search_refs(root, opts, &nrefs, &nvaults);

    check(nrefs == 2, "S1: loaded 2 refs");
    check(hits.size() == 2, "S1: both refs scored > 0");
    const auto* tagged = find_hit(hits, "ref-tagged.md");
    const auto* body = find_hit(hits, "ref-body.md");
    check(tagged != nullptr, "S1: ref-tagged.md surfaced");
    check(body != nullptr, "S1: ref-body.md surfaced");
    check(tagged->score == 5, "S1: tagged ref scores 5 (tag ×5)");
    check(body->score == 1, "S1: body ref scores 1 (content ×1)");
    check(hits[0].path.filename().string() == "ref-tagged.md",
          "S1: tagged ref ranked FIRST (5 > 1)");

    cleanup(root);
}

// ---------------------------------------------------------------------------
// S2: preview = summary: when present, body excerpt when absent
// ---------------------------------------------------------------------------
static void test_s2_preview_prefers_summary() {
    std::cout << "\n=== S2. preview prefers summary:, falls back to body ===\n\n";
    auto root = make_temp_project();

    // Has a summary: — preview must be the summary verbatim, NOT the body.
    write_vault_ref(root, "move-dev", "ref-with-summary.md",
        "---\ntags: [walrus]\n"
        "summary: SUMMARYSENTINEL walrus blob lifecycle decision guide.\n---\n"
        "# Heading\n\nBODYSENTINEL should not be the preview.\n");
    // No summary: — preview must fall back to the body excerpt.
    write_vault_ref(root, "move-dev", "ref-no-summary.md",
        "---\ntags: [walrus]\n---\n# Title\n\n"
        "BODYFALLBACK describes walrus storage here.\n");

    SearchOptions opts;
    opts.query = "walrus";
    opts.limit = 10;
    auto hits = search_refs(root, opts, nullptr, nullptr);

    const auto* with_s = find_hit(hits, "ref-with-summary.md");
    const auto* no_s = find_hit(hits, "ref-no-summary.md");
    check(with_s != nullptr, "S2: ref-with-summary.md surfaced");
    check(no_s != nullptr, "S2: ref-no-summary.md surfaced");

    check(with_s->preview.find("SUMMARYSENTINEL") != std::string::npos,
          "S2: summary-bearing ref previews the summary: field");
    check(with_s->preview.find("BODYSENTINEL") == std::string::npos,
          "S2: summary-bearing ref does NOT preview the body");
    check(no_s->preview.find("BODYFALLBACK") != std::string::npos,
          "S2: summary-less ref previews the body excerpt");

    cleanup(root);
}

// ---------------------------------------------------------------------------
// S3: --agent scopes the corpus to that vault only
// ---------------------------------------------------------------------------
static void test_s3_agent_scoping() {
    std::cout << "\n=== S3. --agent restricts the corpus to one vault ===\n\n";
    auto root = make_temp_project();

    write_vault_ref(root, "move-dev", "ref-movedev-walrus.md",
        "# move-dev\n\nwalrus content in the move-dev vault.\n");
    write_vault_ref(root, "scribe", "ref-scribe-walrus.md",
        "# scribe\n\nwalrus content in the scribe vault.\n");
    write_project_ref(root, "ref-project-walrus.md",
        "# project\n\nwalrus content in the project-scope dir.\n");

    // Unscoped: all three vaults/dirs in the corpus.
    {
        SearchOptions opts;
        opts.query = "walrus";
        opts.limit = 10;
        size_t nrefs = 0, nvaults = 0;
        auto hits = search_refs(root, opts, &nrefs, &nvaults);
        check(nrefs == 3, "S3: unscoped loads all 3 refs");
        check(hits.size() == 3, "S3: unscoped surfaces all 3");
    }

    // Scoped to move-dev: only that vault's ref.
    {
        SearchOptions opts;
        opts.query = "walrus";
        opts.agent = "move-dev";
        opts.limit = 10;
        size_t nrefs = 0, nvaults = 0;
        auto hits = search_refs(root, opts, &nrefs, &nvaults);
        check(nrefs == 1, "S3: --agent move-dev loads only 1 ref");
        check(nvaults == 1, "S3: --agent move-dev searches 1 vault dir");
        check(hits.size() == 1, "S3: --agent move-dev surfaces 1 hit");
        check(find_hit(hits, "ref-movedev-walrus.md") != nullptr,
              "S3: the surfaced hit is the move-dev ref");
        check(find_hit(hits, "ref-scribe-walrus.md") == nullptr,
              "S3: scribe ref excluded under --agent move-dev");
        check(find_hit(hits, "ref-project-walrus.md") == nullptr,
              "S3: project ref excluded under --agent move-dev");
    }

    cleanup(root);
}

// ---------------------------------------------------------------------------
// S4: empty / stopword-only query → no hits, no crash
// ---------------------------------------------------------------------------
static void test_s4_empty_and_stopword_query() {
    std::cout << "\n=== S4. empty / stopword-only query → no hits, no crash ===\n\n";
    auto root = make_temp_project();

    write_vault_ref(root, "move-dev", "ref-anything.md",
        "---\ntags: [walrus]\n---\n# Note\n\nwalrus body.\n");

    // Empty query.
    {
        SearchOptions opts;
        opts.query = "";
        opts.limit = 10;
        auto hits = search_refs(root, opts, nullptr, nullptr);
        check(hits.empty(), "S4: empty query yields no hits");
    }
    // Stopword-only query (tokenizer drops "the"/"of"/"to" + sub-2-char).
    {
        SearchOptions opts;
        opts.query = "the of to a";
        opts.limit = 10;
        auto hits = search_refs(root, opts, nullptr, nullptr);
        check(hits.empty(), "S4: stopword-only query yields no hits");
    }

    cleanup(root);
}

// ---------------------------------------------------------------------------
// S5: corpus spans EVERY vault subdir with a knowledge/ dir + project-scope
// ---------------------------------------------------------------------------
static void test_s5_all_vaults_searchable() {
    std::cout << "\n=== S5. corpus = every vault knowledge/ dir + project ===\n\n";
    auto root = make_temp_project();

    // A "custom" / doer-style vault name that is NOT one of the known knowers.
    write_vault_ref(root, "my-custom-doer", "ref-custom-deepbook.md",
        "# Custom\n\ndeepbook matching engine notes.\n");
    write_vault_ref(root, "architect", "ref-arch-deepbook.md",
        "# Architect\n\ndeepbook layering notes.\n");
    write_project_ref(root, "ref-proj-deepbook.md",
        "# Project\n\ndeepbook promoted ref.\n");

    SearchOptions opts;
    opts.query = "deepbook";
    opts.limit = 10;
    size_t nrefs = 0, nvaults = 0;
    auto hits = search_refs(root, opts, &nrefs, &nvaults);

    check(nrefs == 3, "S5: loaded all 3 refs (custom vault NOT filtered out)");
    // 2 vault knowledge dirs + 1 project knowledge dir = 3 corpus dirs.
    check(nvaults == 3, "S5: searched 3 corpus dirs (2 vaults + project)");
    check(find_hit(hits, "ref-custom-deepbook.md") != nullptr,
          "S5: custom/doer vault ref is searchable");
    check(find_hit(hits, "ref-arch-deepbook.md") != nullptr,
          "S5: known-knower vault ref is searchable");
    check(find_hit(hits, "ref-proj-deepbook.md") != nullptr,
          "S5: project-scope ref is searchable");

    cleanup(root);
}

int main() {
    std::cout << "=====================================================\n";
    std::cout << "  Phase 15 — quorum search (deterministic ref search)\n";
    std::cout << "=====================================================\n";

    test_s1_tag_outranks_body();
    test_s2_preview_prefers_summary();
    test_s3_agent_scoping();
    test_s4_empty_and_stopword_query();
    test_s5_all_vaults_searchable();

    std::cout << "\n---------------------------------------------------\n";
    std::cout << "  passed: " << g_passed << "  failed: " << g_failed << "\n";
    std::cout << "---------------------------------------------------\n";

    return g_failed == 0 ? 0 : 1;
}
