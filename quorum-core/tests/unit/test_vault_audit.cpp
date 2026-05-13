// tests/unit/test_vault_audit.cpp
// Phase 10 Track 4 — quorum vault audit CLI: parse_frontmatter_field helper,
// ISO date parsing, stale/expired classification, --days threshold, unfielded
// skip, both-reasons rendering, invalid-path error.
//
// Dates in test fixtures are computed RELATIVE to std::time(nullptr) at test
// run time (no injection point — plan Q4). This keeps the test valid across
// calendar years. Day-granularity skew during test execution is irrelevant.
//
// Run: ctest -R test_vault_audit --output-on-failure

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <unistd.h>

#include "cli/vault_audit.h"
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
        ("quorum_test_audit_" + std::to_string(getpid()) + "_" +
         std::to_string(g_test_num++));
    fs::create_directories(dir);
    return dir.string();
}

static void cleanup(const std::string& p) { fs::remove_all(p); }

static void write_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path);
    out << content;
}

// Format a date that is `days_offset` days from today (negative = past,
// positive = future) as YYYY-MM-DD in UTC.
static std::string date_offset(long days_offset) {
    std::time_t now = std::time(nullptr);
    std::time_t t = now + days_offset * 86400;
    std::tm utc = *std::gmtime(&t);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &utc);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// T1 — parse_frontmatter_field handles scalar fields
// ---------------------------------------------------------------------------
static void test_t1_parse_frontmatter_field() {
    std::cout << "\n=== T1. parse_frontmatter_field handles scalar fields ===\n\n";
    using sui::quorum::parse_frontmatter_field;

    // Bare value
    std::string c1 = "---\nlast_reviewed: 2026-05-13\n---\n\nbody\n";
    check(parse_frontmatter_field(c1, "last_reviewed") == "2026-05-13",
          "T1: bare date value");

    // Quoted value
    std::string c2 = "---\nlast_reviewed: \"2026-05-13\"\n---\n\nbody\n";
    check(parse_frontmatter_field(c2, "last_reviewed") == "2026-05-13",
          "T1: double-quoted date value");

    // Leading indent
    std::string c3 = "---\n  last_reviewed: 2026-05-13\n---\n";
    check(parse_frontmatter_field(c3, "last_reviewed") == "2026-05-13",
          "T1: indented field");

    // Absent field
    std::string c4 = "---\ntags: [a, b]\n---\n";
    check(parse_frontmatter_field(c4, "last_reviewed") == "",
          "T1: absent field returns empty");

    // No frontmatter
    std::string c5 = "no frontmatter here\n";
    check(parse_frontmatter_field(c5, "last_reviewed") == "",
          "T1: no frontmatter returns empty");

    // Substring collision: last_reviewed_by must NOT match last_reviewed
    std::string c6 = "---\nlast_reviewed_by: alice\n---\n";
    check(parse_frontmatter_field(c6, "last_reviewed") == "",
          "T1: substring collision (last_reviewed_by vs last_reviewed) returns empty");

    // Field outside frontmatter block (in body) — must NOT match
    std::string c7 = "---\ntags: [a]\n---\n\nlast_reviewed: 2026-05-13\n";
    check(parse_frontmatter_field(c7, "last_reviewed") == "",
          "T1: field in body (outside frontmatter) returns empty");

    // Empty value
    std::string c8 = "---\nlast_reviewed:\n---\n";
    check(parse_frontmatter_field(c8, "last_reviewed") == "",
          "T1: empty value returns empty");

    // Multiple fields, query the second
    std::string c9 = "---\ntitle: hi\nexpiry: 2026-12-31\n---\n";
    check(parse_frontmatter_field(c9, "expiry") == "2026-12-31",
          "T1: second field of multi-field frontmatter");
}

// ---------------------------------------------------------------------------
// T2 — parse_iso_date parses ISO dates
// ---------------------------------------------------------------------------
static void test_t2_parse_iso_date() {
    std::cout << "\n=== T2. parse_iso_date parses ISO dates ===\n\n";
    using sui::quorum::cli::detail::parse_iso_date;

    check(parse_iso_date("2026-05-13").has_value(), "T2: valid date parses");
    check(parse_iso_date("2026-05-13T10:00:00Z").has_value(),
          "T2: datetime with time suffix parses (prefix only)");
    check(!parse_iso_date("not-a-date").has_value(),
          "T2: garbage returns nullopt");
    check(!parse_iso_date("").has_value(), "T2: empty string returns nullopt");
    check(!parse_iso_date("2026-13-01").has_value(),
          "T2: month > 12 returns nullopt");
    check(!parse_iso_date("2026-05-32").has_value(),
          "T2: day > 31 returns nullopt");
    check(!parse_iso_date("1969-12-31").has_value(),
          "T2: year < 1970 returns nullopt");
}

// ---------------------------------------------------------------------------
// T3 — Stale last_reviewed surfaces; fresh does not
// ---------------------------------------------------------------------------
static void test_t3_stale_last_reviewed() {
    std::cout << "\n=== T3. Stale last_reviewed surfaces; fresh does not ===\n\n";
    using sui::quorum::cli::VaultAuditOptions;
    using sui::quorum::cli::run_vault_audit;

    auto vault = make_temp_vault();
    write_file(fs::path(vault) / "rule-stale.md",
        "---\nlast_reviewed: " + date_offset(-100) + "\n---\n\nstale content\n");
    write_file(fs::path(vault) / "rule-fresh.md",
        "---\nlast_reviewed: " + date_offset(-30) + "\n---\n\nfresh content\n");

    std::stringstream captured;
    auto* saved = std::cout.rdbuf(captured.rdbuf());

    VaultAuditOptions opts;
    opts.vault_path = vault;
    opts.days = 90;
    int rc = run_vault_audit(opts);

    std::cout.rdbuf(saved);
    auto out = captured.str();

    check(rc == 0, "T3: audit returned 0");
    check(out.find("rule-stale.md") != std::string::npos,
          "T3: stale file surfaces");
    check(out.find("rule-fresh.md") == std::string::npos,
          "T3: fresh file does not surface");
    check(out.find("1 file(s) flagged out of 2 scanned") != std::string::npos,
          "T3: summary counts 1 flagged of 2 scanned");

    cleanup(vault);
}

// ---------------------------------------------------------------------------
// T4 — Expired expiry surfaces; future expiry does not
// ---------------------------------------------------------------------------
static void test_t4_expired_surfaces() {
    std::cout << "\n=== T4. Expired expiry surfaces; future expiry does not ===\n\n";
    using sui::quorum::cli::VaultAuditOptions;
    using sui::quorum::cli::run_vault_audit;

    auto vault = make_temp_vault();
    write_file(fs::path(vault) / "rule-expired.md",
        "---\nexpiry: " + date_offset(-5) + "\n---\n\nexpired\n");
    write_file(fs::path(vault) / "rule-future.md",
        "---\nexpiry: " + date_offset(30) + "\n---\n\nstill good\n");

    std::stringstream captured;
    auto* saved = std::cout.rdbuf(captured.rdbuf());
    VaultAuditOptions opts;
    opts.vault_path = vault;
    opts.days = 90;
    int rc = run_vault_audit(opts);
    std::cout.rdbuf(saved);
    auto out = captured.str();

    check(rc == 0, "T4: audit returned 0");
    check(out.find("rule-expired.md") != std::string::npos,
          "T4: expired file surfaces");
    check(out.find("rule-future.md") == std::string::npos,
          "T4: future-expiry file does not surface");
    check(out.find("Expired (expiry past)") != std::string::npos,
          "T4: 'Expired' section header present");

    cleanup(vault);
}

// ---------------------------------------------------------------------------
// T5 — Files with no audit fields are silently skipped
// ---------------------------------------------------------------------------
static void test_t5_unfielded_skipped() {
    std::cout << "\n=== T5. Files with no audit fields are silently skipped ===\n\n";
    using sui::quorum::cli::VaultAuditOptions;
    using sui::quorum::cli::run_vault_audit;

    auto vault = make_temp_vault();
    write_file(fs::path(vault) / "rule-no-fields.md",
        "no frontmatter at all\njust body text\n");
    write_file(fs::path(vault) / "rule-other-frontmatter.md",
        "---\ntags: [foo, bar]\n---\n\nbody\n");

    std::stringstream captured;
    auto* saved = std::cout.rdbuf(captured.rdbuf());
    VaultAuditOptions opts;
    opts.vault_path = vault;
    opts.days = 90;
    int rc = run_vault_audit(opts);
    std::cout.rdbuf(saved);
    auto out = captured.str();

    check(rc == 0, "T5: audit returned 0");
    check(out.find("rule-no-fields.md") == std::string::npos,
          "T5: file with no frontmatter does not surface");
    check(out.find("rule-other-frontmatter.md") == std::string::npos,
          "T5: file with only tags does not surface");
    check(out.find("0 file(s) flagged out of 2 scanned") != std::string::npos,
          "T5: summary counts 0 flagged of 2 scanned");

    cleanup(vault);
}

// ---------------------------------------------------------------------------
// T6 — File flagged for both reasons appears under "Stale + Expired"
// ---------------------------------------------------------------------------
static void test_t6_both_reasons() {
    std::cout << "\n=== T6. File with both stale + expired surfaces once with both reasons ===\n\n";
    using sui::quorum::cli::VaultAuditOptions;
    using sui::quorum::cli::run_vault_audit;

    auto vault = make_temp_vault();
    write_file(fs::path(vault) / "rule-both.md",
        "---\nlast_reviewed: " + date_offset(-120) +
        "\nexpiry: " + date_offset(-5) + "\n---\n\nboth\n");

    std::stringstream captured;
    auto* saved = std::cout.rdbuf(captured.rdbuf());
    VaultAuditOptions opts;
    opts.vault_path = vault;
    opts.days = 90;
    int rc = run_vault_audit(opts);
    std::cout.rdbuf(saved);
    auto out = captured.str();

    check(rc == 0, "T6: audit returned 0");
    check(out.find("Stale + Expired") != std::string::npos,
          "T6: 'Stale + Expired' section header present");
    // Single mention of the filename (in the both-section). Count occurrences.
    size_t count = 0;
    size_t pos = 0;
    while ((pos = out.find("rule-both.md", pos)) != std::string::npos) {
        ++count;
        ++pos;
    }
    check(count == 1, "T6: filename appears exactly once in output");
    check(out.find("1 file(s) flagged out of 1 scanned") != std::string::npos,
          "T6: summary counts 1 flagged of 1 scanned");

    cleanup(vault);
}

// ---------------------------------------------------------------------------
// T7 — --days flag controls staleness threshold
// ---------------------------------------------------------------------------
static void test_t7_days_flag() {
    std::cout << "\n=== T7. --days flag controls staleness threshold ===\n\n";
    using sui::quorum::cli::VaultAuditOptions;
    using sui::quorum::cli::run_vault_audit;

    auto vault = make_temp_vault();
    write_file(fs::path(vault) / "rule-70days.md",
        "---\nlast_reviewed: " + date_offset(-70) + "\n---\n\nseventy\n");

    // At default 90 days: 70 is NOT stale.
    {
        std::stringstream captured;
        auto* saved = std::cout.rdbuf(captured.rdbuf());
        VaultAuditOptions opts;
        opts.vault_path = vault;
        opts.days = 90;
        int rc = run_vault_audit(opts);
        std::cout.rdbuf(saved);
        check(rc == 0, "T7: audit at days=90 returned 0");
        auto out = captured.str();
        check(out.find("rule-70days.md") == std::string::npos,
              "T7: 70-day-old file NOT flagged at threshold 90");
    }

    // At --days 60: 70 IS stale.
    {
        std::stringstream captured;
        auto* saved = std::cout.rdbuf(captured.rdbuf());
        VaultAuditOptions opts;
        opts.vault_path = vault;
        opts.days = 60;
        int rc = run_vault_audit(opts);
        std::cout.rdbuf(saved);
        check(rc == 0, "T7: audit at days=60 returned 0");
        auto out = captured.str();
        check(out.find("rule-70days.md") != std::string::npos,
              "T7: 70-day-old file IS flagged at threshold 60");
    }

    cleanup(vault);
}

// ---------------------------------------------------------------------------
// T8 — Invalid vault paths produce errors
// ---------------------------------------------------------------------------
static void test_t8_invalid_vault_path() {
    std::cout << "\n=== T8. Invalid vault paths produce errors ===\n\n";
    using sui::quorum::cli::VaultAuditOptions;
    using sui::quorum::cli::run_vault_audit;

    std::stringstream sink;
    auto* saved = std::cerr.rdbuf(sink.rdbuf());

    VaultAuditOptions empty_opts;
    empty_opts.vault_path = "";
    int rc1 = run_vault_audit(empty_opts);
    check(rc1 != 0, "T8: empty vault path -> non-zero");
    check(sink.str().find("empty vault path") != std::string::npos,
          "T8: error message mentions empty vault path");

    sink.str("");
    VaultAuditOptions ghost_opts;
    ghost_opts.vault_path = "/tmp/quorum-audit-does-not-exist-xyz-12345";
    int rc2 = run_vault_audit(ghost_opts);
    check(rc2 == 1, "T8: missing vault path -> exit 1");
    check(sink.str().find("does not exist") != std::string::npos,
          "T8: error message mentions does-not-exist");

    std::cerr.rdbuf(saved);
}

int main() {
    std::cout << "=====================================================\n";
    std::cout << "  Phase 10 Track 4 - quorum vault audit tests\n";
    std::cout << "=====================================================\n";
    test_t1_parse_frontmatter_field();
    test_t2_parse_iso_date();
    test_t3_stale_last_reviewed();
    test_t4_expired_surfaces();
    test_t5_unfielded_skipped();
    test_t6_both_reasons();
    test_t7_days_flag();
    test_t8_invalid_vault_path();
    std::cout << "\npassed: " << g_passed << "  failed: " << g_failed << "\n";
    return g_failed == 0 ? 0 : 1;
}
