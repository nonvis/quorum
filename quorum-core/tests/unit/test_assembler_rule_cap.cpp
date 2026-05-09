// tests/unit/test_assembler_rule_cap.cpp
// Phase 7 Track 1 / Track 6 #19 — filename-aware vault loader.
//
// Verifies that context_assembler:
//   - preloads rule-*.md sorted by recency, capped at MAX_RULES (10)
//   - emits a transparency note when eviction happens
//   - does NOT preload ref-*.md (search-on-demand, Track 4)
//   - keeps plain knowledge files on the recency-budget path, after rules
//   - tolerates an empty knowledge directory
//
// Run:  cd build && cmake .. && make test_assembler_rule_cap && ./test_assembler_rule_cap

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <unistd.h>

#include "agent/context_assembler.h"

namespace fs = std::filesystem;

// --- helpers ----------------------------------------------------------------

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
        ("quorum_test_rulecap_" + std::to_string(getpid()) + "_" + std::to_string(g_test_num++));
    fs::create_directories(dir);
    fs::create_directories(dir / "knowledge");
    return dir.string();
}

static void cleanup(const std::string& path) {
    fs::remove_all(path);
}

// Write a knowledge file with content. Sets mtime via a small sleep so each
// file gets a distinct, ordered timestamp (filename + content uniquely tagged).
static void write_knowledge(const std::string& vault, const std::string& name,
                            const std::string& content) {
    auto path = fs::path(vault) / "knowledge" / name;
    {
        std::ofstream out(path);
        out << content;
    }
    // Ensure mtime ordering on filesystems with coarse timestamp resolution.
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
}

// --- Test A: 15 rule files → 10 loaded, 5 evicted, transparency note --------

static void test_rule_cap_eviction() {
    std::cout << "\n=== A. 15 rule files → 10 most-recent loaded, 5 omitted ===\n\n";

    auto vault = make_temp_vault();

    // Write 15 rule files in ascending recency. Last-written = most recent.
    for (int i = 0; i < 15; ++i) {
        auto idx = std::to_string(i);
        write_knowledge(vault, "rule-" + idx + ".md",
                        "RULE_BODY_" + idx + "\n");
    }

    sui::quorum::ContextAssembler assembler;
    auto prompt = assembler.assemble("test-agent", vault, "turn", "do something");

    // 10 most-recent rules (indices 5..14) must appear; oldest 5 (0..4) must not.
    for (int i = 5; i < 15; ++i) {
        auto needle = "RULE_BODY_" + std::to_string(i) + "\n";
        check(prompt.find(needle) != std::string::npos,
              ("A: rule-" + std::to_string(i) + " (recent) is loaded").c_str());
    }
    for (int i = 0; i < 5; ++i) {
        auto needle = "RULE_BODY_" + std::to_string(i) + "\n";
        check(prompt.find(needle) == std::string::npos,
              ("A: rule-" + std::to_string(i) + " (evicted) is not loaded").c_str());
    }

    check(prompt.find("[5 rules omitted") != std::string::npos,
          "A: transparency note '[5 rules omitted' is present");

    cleanup(vault);
}

// --- Test B: 5 rules + 5 plains → all load, no transparency note ------------

static void test_no_eviction_mixed() {
    std::cout << "\n=== B. 5 rules + 5 plains → all load, no transparency note ===\n\n";

    auto vault = make_temp_vault();

    for (int i = 0; i < 5; ++i) {
        auto idx = std::to_string(i);
        write_knowledge(vault, "rule-" + idx + ".md", "RULE_BODY_" + idx + "\n");
    }
    for (int i = 0; i < 5; ++i) {
        auto idx = std::to_string(i);
        write_knowledge(vault, "note-" + idx + ".md", "PLAIN_BODY_" + idx + "\n");
    }

    sui::quorum::ContextAssembler assembler;
    auto prompt = assembler.assemble("test-agent", vault, "turn", "do something");

    for (int i = 0; i < 5; ++i) {
        auto rule = "RULE_BODY_" + std::to_string(i) + "\n";
        auto plain = "PLAIN_BODY_" + std::to_string(i) + "\n";
        check(prompt.find(rule) != std::string::npos,
              ("B: rule-" + std::to_string(i) + " is loaded").c_str());
        check(prompt.find(plain) != std::string::npos,
              ("B: note-" + std::to_string(i) + " (plain) is loaded").c_str());
    }
    check(prompt.find("rules omitted") == std::string::npos,
          "B: no transparency note when nothing evicted");

    cleanup(vault);
}

// --- Test C: ref-*.md only → none preloaded ---------------------------------

static void test_refs_not_preloaded() {
    std::cout << "\n=== C. ref-*.md files are NOT preloaded ===\n\n";

    auto vault = make_temp_vault();

    for (int i = 0; i < 4; ++i) {
        auto idx = std::to_string(i);
        write_knowledge(vault, "ref-" + idx + ".md", "REF_BODY_" + idx + "\n");
    }

    sui::quorum::ContextAssembler assembler;
    auto prompt = assembler.assemble("test-agent", vault, "turn", "do something");

    for (int i = 0; i < 4; ++i) {
        auto needle = "REF_BODY_" + std::to_string(i) + "\n";
        check(prompt.find(needle) == std::string::npos,
              ("C: ref-" + std::to_string(i) + " is not preloaded").c_str());
    }
    // No knowledge headers either — refs shouldn't even introduce a section.
    check(prompt.find("# Knowledge: ref-") == std::string::npos,
          "C: no '# Knowledge: ref-' header in prompt");

    cleanup(vault);
}

// --- Test D: 3 rules + 3 refs + 3 plains, ordering rules-then-plains --------

static void test_mixed_ordering() {
    std::cout << "\n=== D. mixed vault: rules-then-plains, refs absent ===\n\n";

    auto vault = make_temp_vault();

    // Write rules first, then refs, then plains. With recency-DESC sort,
    // plains will be the most-recent in their bucket — but the assembler
    // orders BUCKETS as rules → plains regardless of recency across buckets.
    for (int i = 0; i < 3; ++i) {
        write_knowledge(vault, "rule-" + std::to_string(i) + ".md",
                        "RULE_BODY_" + std::to_string(i) + "\n");
    }
    for (int i = 0; i < 3; ++i) {
        write_knowledge(vault, "ref-" + std::to_string(i) + ".md",
                        "REF_BODY_" + std::to_string(i) + "\n");
    }
    for (int i = 0; i < 3; ++i) {
        write_knowledge(vault, "note-" + std::to_string(i) + ".md",
                        "PLAIN_BODY_" + std::to_string(i) + "\n");
    }

    sui::quorum::ContextAssembler assembler;
    auto prompt = assembler.assemble("test-agent", vault, "turn", "do something");

    // Rules and plains both present.
    for (int i = 0; i < 3; ++i) {
        auto rule = "RULE_BODY_" + std::to_string(i) + "\n";
        auto plain = "PLAIN_BODY_" + std::to_string(i) + "\n";
        check(prompt.find(rule) != std::string::npos,
              ("D: rule-" + std::to_string(i) + " loaded").c_str());
        check(prompt.find(plain) != std::string::npos,
              ("D: note-" + std::to_string(i) + " loaded").c_str());
    }
    // Refs absent.
    for (int i = 0; i < 3; ++i) {
        auto ref = "REF_BODY_" + std::to_string(i) + "\n";
        check(prompt.find(ref) == std::string::npos,
              ("D: ref-" + std::to_string(i) + " not preloaded").c_str());
    }

    // Bucket ordering: every rule body must appear before every plain body.
    size_t last_rule_pos = 0;
    for (int i = 0; i < 3; ++i) {
        last_rule_pos = std::max(
            last_rule_pos,
            prompt.find("RULE_BODY_" + std::to_string(i) + "\n"));
    }
    size_t first_plain_pos = std::string::npos;
    for (int i = 0; i < 3; ++i) {
        first_plain_pos = std::min(
            first_plain_pos,
            prompt.find("PLAIN_BODY_" + std::to_string(i) + "\n"));
    }
    check(last_rule_pos < first_plain_pos,
          "D: all rules appear before any plain (bucket ordering)");

    // No eviction here — 3 rules under MAX_RULES = 10.
    check(prompt.find("rules omitted") == std::string::npos,
          "D: no transparency note (no eviction)");

    cleanup(vault);
}

// --- Test E: empty knowledge dir doesn't crash, no knowledge section --------

static void test_empty_knowledge_dir() {
    std::cout << "\n=== E. empty knowledge/ dir → no crash, no knowledge section ===\n\n";

    auto vault = make_temp_vault();
    // knowledge/ exists but is empty (created by make_temp_vault()).

    sui::quorum::ContextAssembler assembler;
    auto prompt = assembler.assemble("test-agent", vault, "turn", "do something");

    check(!prompt.empty(), "E: prompt is non-empty (task block still present)");
    check(prompt.find("# Knowledge:") == std::string::npos,
          "E: no '# Knowledge:' header when vault is empty");
    check(prompt.find("rules omitted") == std::string::npos,
          "E: no transparency note when no rules at all");
    check(prompt.find("# Current Task") != std::string::npos,
          "E: task block still emitted");

    cleanup(vault);
}

// --- main -------------------------------------------------------------------

int main() {
    std::cout << "===================================================\n";
    std::cout << "  Phase 7 Track 1 — assembler rule cap unit tests\n";
    std::cout << "===================================================\n";

    test_rule_cap_eviction();
    test_no_eviction_mixed();
    test_refs_not_preloaded();
    test_mixed_ordering();
    test_empty_knowledge_dir();

    std::cout << "\n---------------------------------------------------\n";
    std::cout << "  passed: " << g_passed << "  failed: " << g_failed << "\n";
    std::cout << "---------------------------------------------------\n";

    return g_failed == 0 ? 0 : 1;
}
