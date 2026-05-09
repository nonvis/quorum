// tests/unit/test_assembler_rule_cap.cpp
// Phase 7 Track 1+2 / Track 6 #19+#20 — filename-aware vault loader with
// project + agent scope hierarchy.
//
// Verifies that context_assembler:
//   - preloads rule-*.md sorted by recency, capped at MAX_RULES (10)
//   - emits a transparency note when eviction happens
//   - does NOT preload ref-*.md (search-on-demand, Track 4)
//   - keeps plain knowledge files on the recency-budget path, after rules
//   - tolerates an empty knowledge directory
//   - resolves rules across project (.quorum/knowledge/) and agent-vault scopes
//   - applies the cap on the UNION of both scopes
//   - dedups identical-content collisions toward the agent (most-specific) scope
//   - keeps same-filename / different-content rules in BOTH scopes
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

// Project-scope test layout: <root>/.quorum/knowledge/ + <root>/.quorum/vaults/<agent>/knowledge/.
// Returns root path. Both knowledge dirs are pre-created (empty).
struct ScopedLayout {
    std::string root;
    std::string vault_dir;       // <root>/.quorum/vaults/<agent>/
    std::string project_kdir;    // <root>/.quorum/knowledge/
    std::string vault_kdir;      // <root>/.quorum/vaults/<agent>/knowledge/
};

static ScopedLayout make_scoped_layout(const std::string& agent_name = "test-agent") {
    auto root = fs::temp_directory_path() /
        ("quorum_test_scope_" + std::to_string(getpid()) + "_" + std::to_string(g_test_num++));
    ScopedLayout l;
    l.root = root.string();
    l.vault_dir = (root / ".quorum" / "vaults" / agent_name).string();
    l.project_kdir = (root / ".quorum" / "knowledge").string();
    l.vault_kdir = (root / ".quorum" / "vaults" / agent_name / "knowledge").string();
    fs::create_directories(l.project_kdir);
    fs::create_directories(l.vault_kdir);
    return l;
}

// Write a knowledge file directly into a knowledge dir (project or vault).
static void write_kfile(const std::string& kdir, const std::string& name,
                        const std::string& content) {
    auto path = fs::path(kdir) / name;
    {
        std::ofstream out(path);
        out << content;
    }
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

// --- Test F: project scope alone, agent vault empty ------------------------

static void test_project_scope_alone() {
    std::cout << "\n=== F. project scope alone (agent vault empty) ===\n\n";

    auto l = make_scoped_layout("test-agent");

    write_kfile(l.project_kdir, "rule-project.md", "PROJECT_RULE_BODY\n");

    sui::quorum::ContextAssembler assembler;
    auto prompt = assembler.assemble(
        "test-agent", l.vault_dir, "turn", "do something",
        /*team_roster=*/{}, /*skill_file=*/{}, /*project_root=*/l.root);

    check(prompt.find("PROJECT_RULE_BODY\n") != std::string::npos,
          "F: project rule body present in prompt");
    check(prompt.find("# Knowledge: rule-project.md (project)") != std::string::npos,
          "F: project rule has '(project)' scope annotation");
    check(prompt.find("rules omitted") == std::string::npos,
          "F: no eviction note");

    cleanup(l.root);
}

// --- Test G: agent + project, no overlap, both load with annotations -------

static void test_project_plus_agent_no_overlap() {
    std::cout << "\n=== G. 3 project + 3 agent rules (no overlap) ===\n\n";

    auto l = make_scoped_layout("test-agent");

    // Write project rules first (older), then agent rules (newer).
    for (int i = 0; i < 3; ++i) {
        write_kfile(l.project_kdir, "rule-p" + std::to_string(i) + ".md",
                    "PROJECT_BODY_" + std::to_string(i) + "\n");
    }
    for (int i = 0; i < 3; ++i) {
        write_kfile(l.vault_kdir, "rule-a" + std::to_string(i) + ".md",
                    "AGENT_BODY_" + std::to_string(i) + "\n");
    }

    sui::quorum::ContextAssembler assembler;
    auto prompt = assembler.assemble(
        "test-agent", l.vault_dir, "turn", "do something",
        {}, {}, l.root);

    // All 6 must appear, with correct scope annotations.
    for (int i = 0; i < 3; ++i) {
        auto pbody = "PROJECT_BODY_" + std::to_string(i) + "\n";
        auto abody = "AGENT_BODY_" + std::to_string(i) + "\n";
        check(prompt.find(pbody) != std::string::npos,
              ("G: project rule p" + std::to_string(i) + " loaded").c_str());
        check(prompt.find(abody) != std::string::npos,
              ("G: agent rule a" + std::to_string(i) + " loaded").c_str());

        auto phdr = "# Knowledge: rule-p" + std::to_string(i) + ".md (project)";
        auto ahdr = "# Knowledge: rule-a" + std::to_string(i) + ".md (vault: test-agent)";
        check(prompt.find(phdr) != std::string::npos,
              ("G: project rule p" + std::to_string(i) + " annotated '(project)'").c_str());
        check(prompt.find(ahdr) != std::string::npos,
              ("G: agent rule a" + std::to_string(i) + " annotated '(vault: test-agent)'").c_str());
    }

    check(prompt.find("rules omitted") == std::string::npos,
          "G: no eviction note (6 < MAX_RULES)");

    cleanup(l.root);
}

// --- Test H: cap applies to UNION ------------------------------------------

static void test_cap_on_union() {
    std::cout << "\n=== H. cap on union: 8 project + 8 agent → 10 kept, 6 omitted ===\n\n";

    auto l = make_scoped_layout("test-agent");

    // Interleave so recency is mixed across scopes — neither scope dominates
    // the kept set. We write project[0], agent[0], project[1], agent[1], ...
    // so the 10 most-recent will include rules from BOTH scopes.
    for (int i = 0; i < 8; ++i) {
        write_kfile(l.project_kdir, "rule-p" + std::to_string(i) + ".md",
                    "PROJECT_BODY_" + std::to_string(i) + "\n");
        write_kfile(l.vault_kdir, "rule-a" + std::to_string(i) + ".md",
                    "AGENT_BODY_" + std::to_string(i) + "\n");
    }

    sui::quorum::ContextAssembler assembler;
    auto prompt = assembler.assemble(
        "test-agent", l.vault_dir, "turn", "do something",
        {}, {}, l.root);

    // Eviction note: 16 rules - 10 cap = 6 omitted.
    check(prompt.find("[6 rules omitted") != std::string::npos,
          "H: '[6 rules omitted' transparency note present");

    // Count loaded rule headers (one per kept rule across BOTH scopes).
    size_t loaded = 0;
    size_t pos = 0;
    while ((pos = prompt.find("# Knowledge: rule-", pos)) != std::string::npos) {
        ++loaded;
        ++pos;
    }
    check(loaded == 10, "H: exactly 10 rules loaded (MAX_RULES)");

    // Both scopes must be represented in the kept set — recency interleave
    // guarantees neither scope is fully evicted.
    bool project_kept = prompt.find("(project)") != std::string::npos;
    bool agent_kept = prompt.find("(vault: test-agent)") != std::string::npos;
    check(project_kept, "H: at least one project rule survives the cap");
    check(agent_kept, "H: at least one agent rule survives the cap");

    cleanup(l.root);
}

// --- Test I: dedup on identical content — agent wins, NOT a cap eviction ---

static void test_dedup_identical_content() {
    std::cout << "\n=== I. identical content in both scopes → agent wins, silent ===\n\n";

    auto l = make_scoped_layout("test-agent");

    // Same filename, same content — agent copy must win, project copy
    // suppressed without showing up as an eviction.
    write_kfile(l.project_kdir, "rule-foo.md", "do X\n");
    write_kfile(l.vault_kdir, "rule-foo.md", "do X\n");

    sui::quorum::ContextAssembler assembler;
    auto prompt = assembler.assemble(
        "test-agent", l.vault_dir, "turn", "do something",
        {}, {}, l.root);

    // Body appears exactly once.
    size_t first = prompt.find("do X\n");
    check(first != std::string::npos, "I: agent rule body appears");
    check(prompt.find("do X\n", first + 1) == std::string::npos,
          "I: rule body appears exactly once (project copy suppressed)");

    // The single header must be the agent-vault flavor.
    check(prompt.find("# Knowledge: rule-foo.md (vault: test-agent)") != std::string::npos,
          "I: agent-scope header present");
    check(prompt.find("# Knowledge: rule-foo.md (project)") == std::string::npos,
          "I: project-scope header suppressed");

    // Dedup is NOT a cap eviction.
    check(prompt.find("rules omitted") == std::string::npos,
          "I: dedup is silent — no eviction note");

    cleanup(l.root);
}

// --- Test J: same filename, different content → both load, annotations split

static void test_same_filename_different_content() {
    std::cout << "\n=== J. same filename, different content → BOTH load ===\n\n";

    auto l = make_scoped_layout("test-agent");

    write_kfile(l.project_kdir, "rule-foo.md", "do X\n");
    write_kfile(l.vault_kdir, "rule-foo.md", "do Y\n");

    sui::quorum::ContextAssembler assembler;
    auto prompt = assembler.assemble(
        "test-agent", l.vault_dir, "turn", "do something",
        {}, {}, l.root);

    check(prompt.find("do X\n") != std::string::npos,
          "J: project content 'do X' present");
    check(prompt.find("do Y\n") != std::string::npos,
          "J: agent content 'do Y' present");

    // Both scope annotations distinguish them despite the shared filename.
    check(prompt.find("# Knowledge: rule-foo.md (project)") != std::string::npos,
          "J: project-scope header present");
    check(prompt.find("# Knowledge: rule-foo.md (vault: test-agent)") != std::string::npos,
          "J: agent-scope header present");

    check(prompt.find("rules omitted") == std::string::npos,
          "J: no eviction note (only 2 rules)");

    cleanup(l.root);
}

// --- main -------------------------------------------------------------------

int main() {
    std::cout << "===================================================\n";
    std::cout << "  Phase 7 Track 1+2 — assembler scope/cap unit tests\n";
    std::cout << "===================================================\n";

    test_rule_cap_eviction();
    test_no_eviction_mixed();
    test_refs_not_preloaded();
    test_mixed_ordering();
    test_empty_knowledge_dir();
    test_project_scope_alone();
    test_project_plus_agent_no_overlap();
    test_cap_on_union();
    test_dedup_identical_content();
    test_same_filename_different_content();

    std::cout << "\n---------------------------------------------------\n";
    std::cout << "  passed: " << g_passed << "  failed: " << g_failed << "\n";
    std::cout << "---------------------------------------------------\n";

    return g_failed == 0 ? 0 : 1;
}
