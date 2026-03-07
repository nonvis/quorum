// tests/unit/test_inbox_writer.cpp
// Inline test for InboxWriter — compile via CMake target test_inbox_writer.
// Run:  cd build && ctest --output-on-failure  (or ./test_inbox_writer)

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

#include "knowledge/inbox_writer.h"

// ─── helpers ─────────────────────────────────────────────────────────────────

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        std::exit(1);
    }
    std::cout << "[PASS] " << msg << "\n";
}

// Read entire file into string
static std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) return {};
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
}

// Create a unique temp directory for testing
static std::filesystem::path make_temp_dir() {
    auto base = std::filesystem::temp_directory_path() / "test_inbox_writer_XXXXXX";
    auto tmpl = base.string();
    // mkdtemp modifies the template in-place
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* result = mkdtemp(buf.data());
    if (!result) {
        std::cerr << "Failed to create temp directory\n";
        std::exit(1);
    }
    return std::filesystem::path(result);
}

// ─── tests ───────────────────────────────────────────────────────────────────

static void test_init() {
    auto tmp = make_temp_dir();
    auto knowledge = tmp / "knowledge";

    sui::quorum::InboxWriter writer(knowledge.string());

    bool ok = writer.init();
    check(ok, "init: returns true");

    check(std::filesystem::is_directory(knowledge / "inbox"),
          "init: inbox/ created");
    check(std::filesystem::is_directory(knowledge / "library"),
          "init: library/ created");
    check(std::filesystem::is_directory(knowledge / "archive"),
          "init: archive/ created");

    // Idempotent — calling init again should succeed
    bool ok2 = writer.init();
    check(ok2, "init: idempotent (second call succeeds)");

    std::filesystem::remove_all(tmp);
}

static void test_write_observation() {
    auto tmp = make_temp_dir();
    auto knowledge = tmp / "knowledge";

    sui::quorum::InboxWriter writer(knowledge.string());
    (void)writer.init();

    sui::quorum::ParsedObservation obs;
    obs.title     = "Session 30 Adverse Selection Spike";
    obs.agent     = "market_analyst";
    obs.task_type = "daily_review";
    obs.tags      = {"mm-bot", "adverse-selection", "session-30"};
    obs.content   = "15 adverse fills totaling -4.8 SUI.\nSpread of 30bps insufficient during high-vol windows.";

    bool ok = writer.write_observation(obs);
    check(ok, "write: returns true");

    // Find the file in inbox/
    auto inbox = knowledge / "inbox";
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(inbox)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path().filename().string());
        }
    }

    check(files.size() == 1, "write: exactly one file in inbox/");

    auto& fname = files[0];
    // Filename pattern: YYYY-MM-DD_HHMMSS_market_analyst_daily_review.md
    check(fname.size() > 15, "write: filename has reasonable length");
    check(fname.substr(4, 1) == "-", "write: filename has date separator at pos 4");
    check(fname.substr(7, 1) == "-", "write: filename has date separator at pos 7");
    check(fname.substr(10, 1) == "_", "write: filename has underscore after date");
    check(fname.find("market_analyst") != std::string::npos,
          "write: filename contains agent name");
    check(fname.find("daily_review") != std::string::npos,
          "write: filename contains task_type");
    check(fname.ends_with(".md"), "write: filename ends with .md");

    // Read and verify content
    auto content = read_file(inbox / fname);
    check(!content.empty(), "write: file has content");

    // Check YAML frontmatter
    check(content.find("---") == 0, "write: starts with frontmatter delimiter");
    check(content.find("agent: market_analyst") != std::string::npos,
          "write: frontmatter has agent");
    check(content.find("task_type: daily_review") != std::string::npos,
          "write: frontmatter has task_type");
    check(content.find("date: ") != std::string::npos,
          "write: frontmatter has date");
    check(content.find("tags: [mm-bot, adverse-selection, session-30]") != std::string::npos,
          "write: frontmatter has tags");
    check(content.find("processed: false") != std::string::npos,
          "write: frontmatter has processed: false");

    // Check body
    check(content.find("# Session 30 Adverse Selection Spike") != std::string::npos,
          "write: has title as H1 heading");
    check(content.find("15 adverse fills") != std::string::npos,
          "write: has observation body text");
    check(content.find("30bps insufficient") != std::string::npos,
          "write: has full content");

    std::filesystem::remove_all(tmp);
}

static void test_write_observation_no_tags() {
    auto tmp = make_temp_dir();
    auto knowledge = tmp / "knowledge";

    sui::quorum::InboxWriter writer(knowledge.string());
    (void)writer.init();

    sui::quorum::ParsedObservation obs;
    obs.title     = "Simple Note";
    obs.agent     = "engineer";
    obs.task_type = "code_review";
    // No tags
    obs.content   = "Nothing noteworthy.";

    bool ok = writer.write_observation(obs);
    check(ok, "no-tags: write returns true");

    // Find and read the file
    auto inbox = knowledge / "inbox";
    std::string content;
    for (const auto& entry : std::filesystem::directory_iterator(inbox)) {
        if (entry.is_regular_file()) {
            content = read_file(entry.path());
            break;
        }
    }

    check(!content.empty(), "no-tags: file has content");
    check(content.find("tags: []") != std::string::npos,
          "no-tags: frontmatter has empty tags list");
    check(content.find("# Simple Note") != std::string::npos,
          "no-tags: has title");
    check(content.find("Nothing noteworthy.") != std::string::npos,
          "no-tags: has content");

    std::filesystem::remove_all(tmp);
}

static void test_write_observation_empty_agent() {
    auto tmp = make_temp_dir();
    auto knowledge = tmp / "knowledge";

    sui::quorum::InboxWriter writer(knowledge.string());
    (void)writer.init();

    sui::quorum::ParsedObservation obs;
    obs.title   = "Unnamed Observation";
    obs.content = "Some content.";
    // agent and task_type left empty

    bool ok = writer.write_observation(obs);
    check(ok, "empty-agent: write returns true");

    // Verify filename uses "unknown" for empty fields
    auto inbox = knowledge / "inbox";
    for (const auto& entry : std::filesystem::directory_iterator(inbox)) {
        if (entry.is_regular_file()) {
            auto fname = entry.path().filename().string();
            check(fname.find("unknown") != std::string::npos,
                  "empty-agent: filename uses 'unknown' for empty agent/task_type");
            break;
        }
    }

    std::filesystem::remove_all(tmp);
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== test_inbox_writer ===\n\n";

    test_init();
    test_write_observation();
    test_write_observation_no_tags();
    test_write_observation_empty_agent();

    std::cout << "\nAll tests passed.\n";
    return 0;
}
