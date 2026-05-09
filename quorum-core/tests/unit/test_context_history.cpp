// tests/unit/test_context_history.cpp
// Unit tests for vault/context_history.h — CONTEXT.md audit-trail writer.
//
// Run:  cd build && cmake .. && make test_context_history && ./test_context_history

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>

#include <unistd.h>

#include "vault/context_history.h"

namespace fs = std::filesystem;

static int g_passed = 0;
static int g_failed = 0;

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        ++g_failed;
        std::exit(1);
    }
    std::cout << "[PASS] " << msg << "\n";
    ++g_passed;
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

static fs::path make_tmp_dir(const std::string& tag) {
    auto base = fs::temp_directory_path() /
        ("quorum_context_history_test_" + tag + "_" + std::to_string(::getpid()));
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

// Count records by counting sentinel occurrences (each record starts
// with a sentinel line at column 0).
static int count_records(const std::string& history_content) {
    int count = 0;
    size_t pos = 0;
    const std::string sentinel = "---QUORUM-HISTORY---";
    while (true) {
        size_t found = history_content.find(sentinel, pos);
        if (found == std::string::npos) break;
        // Sentinel must be at start of file or after a newline.
        bool at_line_start = (found == 0) || (history_content[found - 1] == '\n');
        // Sentinel line must be exactly the sentinel (no trailing chars).
        size_t line_end = history_content.find('\n', found);
        bool exact_line = (line_end == std::string::npos)
            ? (found + sentinel.size() == history_content.size())
            : (line_end - found == sentinel.size());
        if (at_line_start && exact_line) ++count;
        pos = found + sentinel.size();
    }
    return count;
}

// ── Test A: prior content captured to .history on second call ────────────────
static void test_A_basic_history_capture() {
    std::cout << "\n=== A. basic history capture ===\n\n";
    auto tdir = make_tmp_dir("A");
    auto ctx = (tdir / "CONTEXT.md").string();
    auto hist = ctx + ".history";

    sui::quorum::vault::write_context_with_history(ctx, "FIRST\n");
    check(!fs::exists(hist), "A: no .history after first write");

    sui::quorum::vault::write_context_with_history(ctx, "SECOND\n");
    check(fs::exists(hist), "A: .history exists after second write");

    auto h = read_file(hist);
    check(h.find("FIRST") != std::string::npos,
          "A: .history contains prior 'FIRST' content");
    check(h.find("SECOND") == std::string::npos,
          "A: .history does NOT contain current 'SECOND' content");
    check(count_records(h) == 1, "A: exactly one history record");

    auto current = read_file(ctx);
    check(current == "SECOND\n", "A: CONTEXT.md has current content");
}

// ── Test B: 25 sequential writes leave 20 entries, oldest 5 dropped ──────────
static void test_B_cap_at_20() {
    std::cout << "\n=== B. 25 writes -> 20 entries, oldest dropped ===\n\n";
    auto tdir = make_tmp_dir("B");
    auto ctx = (tdir / "CONTEXT.md").string();
    auto hist = ctx + ".history";

    // Write 25 distinct contents. After call N, the file holds content "vN-1"
    // (for N>=1) and history holds the previous N-1 entries.
    for (int i = 0; i < 25; ++i) {
        sui::quorum::vault::write_context_with_history(ctx, "v" + std::to_string(i) + "\n");
    }

    auto h = read_file(hist);
    int n = count_records(h);
    check(n == 20, "B: exactly 20 records after 25 writes");

    // Final write was "v24" — file content. History entries should hold
    // priors v0..v23 (24 of them), trimmed to most recent 20: v4..v23.
    check(h.find("\nv0\n") == std::string::npos && h.find("v0\n") != 0,
          "B: oldest entry 'v0' dropped");
    check(h.find("v1\n") == std::string::npos || h.find("\nv1\n") == std::string::npos,
          "B: 'v1' dropped");
    // Spot-check: v3 should be dropped (oldest 5 = v0..v3 of priors? priors are v0..v23,
    // we keep last 20 = v4..v23 — so v0..v3 are dropped).
    check(h.find("\nv4\n") != std::string::npos, "B: 'v4' retained (oldest kept)");
    check(h.find("\nv23\n") != std::string::npos, "B: 'v23' retained (newest)");
    // Confirm dropped count.
    int dropped = 0;
    for (int i = 0; i < 4; ++i) {
        if (h.find("\nv" + std::to_string(i) + "\n") == std::string::npos) ++dropped;
    }
    check(dropped == 4, "B: priors v0..v3 (4 oldest) all dropped");
}

// ── Test C: first call (no prior file) creates CONTEXT.md, no .history ───────
static void test_C_first_call_no_history() {
    std::cout << "\n=== C. first call, no prior file ===\n\n";
    auto tdir = make_tmp_dir("C");
    auto ctx = (tdir / "CONTEXT.md").string();
    auto hist = ctx + ".history";

    check(!fs::exists(ctx), "C: CONTEXT.md does not exist initially");
    check(!fs::exists(hist), "C: .history does not exist initially");

    sui::quorum::vault::write_context_with_history(ctx, "initial\n");

    check(fs::exists(ctx), "C: CONTEXT.md created on first call");
    check(!fs::exists(hist), "C: .history NOT created on first call");
    check(read_file(ctx) == "initial\n", "C: CONTEXT.md contents correct");
}

// ── Test D: sentinel + ISO8601 timestamp format ──────────────────────────────
static void test_D_sentinel_and_timestamp() {
    std::cout << "\n=== D. sentinel + ISO8601 timestamp ===\n\n";
    auto tdir = make_tmp_dir("D");
    auto ctx = (tdir / "CONTEXT.md").string();
    auto hist = ctx + ".history";

    sui::quorum::vault::write_context_with_history(ctx, "one\n");
    sui::quorum::vault::write_context_with_history(ctx, "two\n");
    sui::quorum::vault::write_context_with_history(ctx, "three\n");

    auto h = read_file(hist);

    // Sentinel: literal `---QUORUM-HISTORY---` at start of a line.
    check(h.find("---QUORUM-HISTORY---") != std::string::npos,
          "D: contains '---QUORUM-HISTORY---' sentinel");
    // Plain `---` markdown HR sentinel must NOT collide: the content
    // we wrote contains no `---`, so any `---` in history must be from
    // sentinel lines specifically.
    check(count_records(h) == 2, "D: exactly 2 records after 3 writes");

    // ISO8601 UTC: YYYY-MM-DDTHH:MM:SSZ — appears on the line after each sentinel.
    std::regex iso_re(R"(## \d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)");
    auto begin = std::sregex_iterator(h.begin(), h.end(), iso_re);
    auto end = std::sregex_iterator();
    int ts_count = static_cast<int>(std::distance(begin, end));
    check(ts_count == 2, "D: exactly 2 ISO8601 timestamp lines");

    // Sentinel doesn't collide with markdown horizontal rules. Inject
    // a CONTEXT.md whose body contains a raw `---` HR and YAML
    // frontmatter delimiters; verify history still parses cleanly.
    auto tdir2 = make_tmp_dir("D2");
    auto ctx2 = (tdir2 / "CONTEXT.md").string();
    auto hist2 = ctx2 + ".history";

    std::string with_hr =
        "---\n"
        "title: test\n"
        "---\n"
        "\n"
        "# Heading\n"
        "\n"
        "Body before HR.\n"
        "\n"
        "---\n"
        "\n"
        "Body after HR.\n";
    sui::quorum::vault::write_context_with_history(ctx2, with_hr);
    sui::quorum::vault::write_context_with_history(ctx2, "next\n");
    auto h2 = read_file(hist2);
    check(count_records(h2) == 1,
          "D: HR/frontmatter `---` lines do not get treated as sentinels");
    check(h2.find("title: test") != std::string::npos,
          "D: prior content (with frontmatter) preserved verbatim in history");
}

// ── main ─────────────────────────────────────────────────────────────────────
int main() {
    std::cout << "=== Context History Unit Tests ===\n";

    test_A_basic_history_capture();
    test_B_cap_at_20();
    test_C_first_call_no_history();
    test_D_sentinel_and_timestamp();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
