// tests/unit/test_vault_dedup.cpp
// Phase 10 Track 3 — quorum vault dedup CLI: Jaccard math, clustering,
// threshold parametrization, frontmatter strip regression guard, dry-run
// no-mutation, invalid-path error.
//
// The interactive prompt path (merge/delete/keep-all) is exercised by the
// manual gate (Phase 10 Sub-gate C / plan Phase D), not here.
//
// Run: ctest -R test_vault_dedup --output-on-failure

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include <unistd.h>

#include "cli/vault_dedup.h"

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
        ("quorum_test_dedup_" + std::to_string(getpid()) + "_" +
         std::to_string(g_test_num++));
    fs::create_directories(dir);
    return dir.string();
}

static void cleanup(const std::string& p) { fs::remove_all(p); }

static void write_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path);
    out << content;
}

// ---------------------------------------------------------------------------
// T1 — Jaccard math sanity
// ---------------------------------------------------------------------------
static void test_t1_jaccard_math() {
    std::cout << "\n=== T1. Jaccard math sanity ===\n\n";
    using sui::quorum::cli::detail::jaccard;
    using sui::quorum::cli::detail::token_set;

    auto a = token_set("foo bar baz qux");
    auto b = token_set("foo bar baz qux");
    check(jaccard(a, b) == 1.0, "T1: identical token sets -> 1.0");

    auto c = token_set("");
    auto d = token_set("");
    check(jaccard(c, d) == 1.0, "T1: both-empty -> 1.0 (convention)");

    auto e = token_set("foo bar");
    auto f = token_set("baz qux");
    check(jaccard(e, f) == 0.0, "T1: disjoint sets -> 0.0");

    // One empty, one non-empty -> 0.0
    check(jaccard(c, e) == 0.0, "T1: empty vs non-empty -> 0.0");
}

// ---------------------------------------------------------------------------
// T2 — Near-duplicate clustering
// ---------------------------------------------------------------------------
static void test_t2_near_duplicate_cluster() {
    std::cout << "\n=== T2. Near-duplicate clustering ===\n\n";
    using sui::quorum::cli::detail::cluster_files;
    using sui::quorum::cli::detail::enumerate_vault;

    auto vault = make_temp_vault();
    write_file(fs::path(vault) / "rule-naming-a.md",
        "Variable names must be snake case. Function names must be snake case. "
        "Type names must be pascal case. Constants must be screaming snake case. "
        "Module names must be snake case.\n");
    write_file(fs::path(vault) / "rule-naming-b.md",
        "Variable names use snake case. Function names use snake case. "
        "Type names use pascal case. Constants use screaming snake case. "
        "Module names use snake case.\n");

    auto files = enumerate_vault(vault);
    check(files.size() == 2, "T2: enumerate found 2 files");

    auto clusters = cluster_files(files, 0.7);
    check(clusters.size() == 1, "T2: 1 cluster formed");
    check(clusters.front().members.size() == 1, "T2: cluster has 1 member (+anchor = 2 files)");
    check(clusters.front().members.front().second > 0.7,
          "T2: member similarity > 0.7");

    cleanup(vault);
}

// ---------------------------------------------------------------------------
// T3 — Below-threshold pairs do NOT cluster
// ---------------------------------------------------------------------------
static void test_t3_below_threshold_no_cluster() {
    std::cout << "\n=== T3. Below-threshold pairs do not cluster ===\n\n";
    using sui::quorum::cli::detail::cluster_files;
    using sui::quorum::cli::detail::enumerate_vault;

    auto vault = make_temp_vault();
    write_file(fs::path(vault) / "rule-deepbook-overview.md",
        "DeepBook is a CLOB on Sui supporting limit orders and market orders. "
        "It provides on-chain matching with deep liquidity.\n");
    write_file(fs::path(vault) / "rule-deepbook-fees.md",
        "Maker fees on DeepBook are zero basis points. Taker fees are five "
        "basis points. Settlement happens in the same epoch.\n");

    auto files = enumerate_vault(vault);
    check(files.size() == 2, "T3: enumerate found 2 files");

    auto clusters = cluster_files(files, 0.7);
    check(clusters.empty(), "T3: no clusters at threshold 0.7");

    cleanup(vault);
}

// ---------------------------------------------------------------------------
// T4 — Singleton (unique) file does not cluster
// ---------------------------------------------------------------------------
static void test_t4_singleton_no_cluster() {
    std::cout << "\n=== T4. Singleton file does not cluster ===\n\n";
    using sui::quorum::cli::detail::cluster_files;
    using sui::quorum::cli::detail::enumerate_vault;

    auto vault = make_temp_vault();
    write_file(fs::path(vault) / "rule-unique.md",
        "Solo content unrelated to anything else.\n");

    auto files = enumerate_vault(vault);
    check(files.size() == 1, "T4: enumerate found 1 file");

    auto clusters = cluster_files(files, 0.7);
    check(clusters.empty(), "T4: no clusters (singleton)");

    cleanup(vault);
}

// ---------------------------------------------------------------------------
// T5 — Dry-run produces no filesystem mutation
// ---------------------------------------------------------------------------
static void test_t5_dry_run_no_mutation() {
    std::cout << "\n=== T5. --dry-run does not mutate filesystem ===\n\n";
    using sui::quorum::cli::run_vault_dedup;
    using sui::quorum::cli::VaultDedupOptions;

    auto vault = make_temp_vault();
    auto pa = fs::path(vault) / "rule-naming-a.md";
    auto pb = fs::path(vault) / "rule-naming-b.md";
    write_file(pa,
        "Variable names must be snake case. Function names must be snake case. "
        "Type names must be pascal case. Constants must be screaming snake case. "
        "Module names must be snake case.\n");
    write_file(pb,
        "Variable names use snake case. Function names use snake case. "
        "Type names use pascal case. Constants use screaming snake case. "
        "Module names use snake case.\n");

    auto count_before = std::distance(fs::directory_iterator(vault),
                                       fs::directory_iterator{});
    auto mtime_a = fs::last_write_time(pa);
    auto mtime_b = fs::last_write_time(pb);

    // Redirect stdout to capture output without polluting test log.
    std::stringstream captured;
    auto* saved = std::cout.rdbuf(captured.rdbuf());

    VaultDedupOptions opts;
    opts.vault_path = vault;
    opts.dry_run = true;
    opts.threshold = 0.7;
    int rc = run_vault_dedup(opts);

    std::cout.rdbuf(saved);

    check(rc == 0, "T5: dry-run returned 0");
    auto count_after = std::distance(fs::directory_iterator(vault),
                                      fs::directory_iterator{});
    check(count_before == count_after, "T5: file count unchanged");
    check(fs::exists(pa) && fs::exists(pb), "T5: both files still present");
    check(fs::last_write_time(pa) == mtime_a, "T5: file A mtime unchanged");
    check(fs::last_write_time(pb) == mtime_b, "T5: file B mtime unchanged");

    auto out = captured.str();
    check(out.find("Cluster 1") != std::string::npos,
          "T5: output contains 'Cluster 1'");
    check(out.find("rule-naming-a.md") != std::string::npos,
          "T5: output mentions rule-naming-a.md");
    check(out.find("rule-naming-b.md") != std::string::npos,
          "T5: output mentions rule-naming-b.md");
    check(out.find("Found 1 cluster") != std::string::npos,
          "T5: output contains 'Found 1 cluster'");

    cleanup(vault);
}

// ---------------------------------------------------------------------------
// T6 — Frontmatter strip regression guard
// ---------------------------------------------------------------------------
static void test_t6_frontmatter_strip() {
    std::cout << "\n=== T6. Frontmatter is stripped before tokenization ===\n\n";
    using sui::quorum::cli::detail::cluster_files;
    using sui::quorum::cli::detail::enumerate_vault;

    auto vault = make_temp_vault();
    write_file(fs::path(vault) / "rule-tagged-a.md",
        "---\ntags: [walrus, seal, sui, knowledge, deepbook]\n---\n\n"
        "# Note A\n\nThis is the body content that should match.\n");
    write_file(fs::path(vault) / "rule-tagged-b.md",
        "---\ntags: [coffee, lasagna, opera]\n---\n\n"
        "# Note B\n\nThis is the body content that should match.\n");

    auto files = enumerate_vault(vault);
    check(files.size() == 2, "T6: enumerate found 2 files");

    auto clusters = cluster_files(files, 0.7);
    check(clusters.size() == 1, "T6: 1 cluster formed despite disjoint tags");
    check(clusters.front().members.size() == 1, "T6: cluster has 1 member");
    check(clusters.front().members.front().second > 0.95,
          "T6: similarity > 0.95 (frontmatter stripped)");

    cleanup(vault);
}

// ---------------------------------------------------------------------------
// T7 — Threshold parametrization
// ---------------------------------------------------------------------------
// Construct two files whose token sets give a Jaccard between 0.7 and 0.8 so
// a 0.7 threshold clusters them and a 0.8 threshold doesn't.
//
// A = {alpha, beta, gamma, delta, epsilon}, B = {alpha, beta, gamma, delta, zeta}
//   intersect = 4, union = 6 -> Jaccard = 4/6 = 0.6667.
//
// Two-thirds isn't between 0.7 and 0.8. Tune set size:
//   A = {alpha, beta, gamma, delta, epsilon, zeta, eta, theta}
//   B = {alpha, beta, gamma, delta, epsilon, zeta, eta, iota}
//   intersect = 7, union = 9 -> Jaccard = 7/9 = 0.7778.
// That's > 0.7 and < 0.8 — perfect.
static void test_t7_threshold_parametrization() {
    std::cout << "\n=== T7. --threshold parametrizes clustering ===\n\n";
    using sui::quorum::cli::detail::cluster_files;
    using sui::quorum::cli::detail::enumerate_vault;
    using sui::quorum::cli::detail::jaccard;
    using sui::quorum::cli::detail::token_set;

    // Direct math check first.
    auto a = token_set("alpha beta gamma delta epsilon zeta eta theta");
    auto b = token_set("alpha beta gamma delta epsilon zeta eta iota");
    double j = jaccard(a, b);
    check(j > 0.7 && j < 0.8, "T7: hand-computed Jaccard is in (0.7, 0.8)");

    auto vault = make_temp_vault();
    write_file(fs::path(vault) / "rule-a.md",
        "alpha beta gamma delta epsilon zeta eta theta\n");
    write_file(fs::path(vault) / "rule-b.md",
        "alpha beta gamma delta epsilon zeta eta iota\n");

    auto files = enumerate_vault(vault);
    check(files.size() == 2, "T7: enumerate found 2 files");

    auto loose = cluster_files(files, 0.7);
    check(loose.size() == 1, "T7: threshold 0.7 -> 1 cluster");

    auto strict = cluster_files(files, 0.8);
    check(strict.empty(), "T7: threshold 0.8 -> 0 clusters");

    cleanup(vault);
}

// ---------------------------------------------------------------------------
// T8 — Invalid vault paths return non-zero from run_vault_dedup
// ---------------------------------------------------------------------------
static void test_t8_invalid_vault_path() {
    std::cout << "\n=== T8. Invalid vault paths produce errors ===\n\n";
    using sui::quorum::cli::run_vault_dedup;
    using sui::quorum::cli::VaultDedupOptions;

    // Capture stderr (cerr) to avoid polluting test output.
    std::stringstream sink;
    auto* saved = std::cerr.rdbuf(sink.rdbuf());

    // Empty path
    VaultDedupOptions empty_opts;
    empty_opts.vault_path = "";
    int rc1 = run_vault_dedup(empty_opts);
    check(rc1 != 0, "T8: empty vault path -> non-zero");
    check(sink.str().find("empty vault path") != std::string::npos,
          "T8: error message mentions empty vault path");

    // Non-existent path
    sink.str("");
    VaultDedupOptions ghost_opts;
    ghost_opts.vault_path = "/tmp/quorum-dedup-does-not-exist-xyz-12345";
    int rc2 = run_vault_dedup(ghost_opts);
    check(rc2 == 1, "T8: missing vault path -> exit 1");
    check(sink.str().find("does not exist") != std::string::npos,
          "T8: error message mentions does-not-exist");

    std::cerr.rdbuf(saved);
}

int main() {
    std::cout << "=====================================================\n";
    std::cout << "  Phase 10 Track 3 - quorum vault dedup tests\n";
    std::cout << "=====================================================\n";
    test_t1_jaccard_math();
    test_t2_near_duplicate_cluster();
    test_t3_below_threshold_no_cluster();
    test_t4_singleton_no_cluster();
    test_t5_dry_run_no_mutation();
    test_t6_frontmatter_strip();
    test_t7_threshold_parametrization();
    test_t8_invalid_vault_path();
    std::cout << "\npassed: " << g_passed << "  failed: " << g_failed << "\n";
    return g_failed == 0 ? 0 : 1;
}
