// tests/unit/test_assembler_pitch.cpp
// Phase 11 Track 4 — ## Project Pitch injection (scribe-respects-Pitch loop).
//
// The assembler injects a condensed `## Project Pitch` digest into the
// SCRIBE's prompt, distilled from the project-root aspirational layer
// (Pitch/00 - Introduction.md "What we're building" / "Current direction"
//  + Pitch/01 - Anti-goals.md "Anti-goals"). The scribe consults it as
// source-of-truth when deciding keep/discard/update/restructure of its own
// rule-*/ref-* knowledge (pitch-protocol.md v0.1).
//
// Asserts:
//   P1 — Pitch files exist + scribe role → '## Project Pitch' with digested
//        content in user_message (NOT system_prompt).
//   P2 — Pitch files absent → no '## Project Pitch' section at all.
//   P3 — non-scribe role (Pitch files present) → no '## Project Pitch'
//        (scribe-only scope).
//   P4 — long section bodies are capped with a trailing "…" marker.
//
// Run:  cd build && ctest -R test_assembler_pitch --output-on-failure

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "agent/context_assembler.h"

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

static void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << content;
}

// Build a project layout with a .quorum/ vault for `agent` and (optionally)
// a project-root Pitch/ aspirational layer.
struct PitchLayout {
    std::string root;
    std::string vault_dir;  // <root>/.quorum/vaults/<agent>/
};

static PitchLayout make_layout(const std::string& agent, bool with_pitch) {
    auto root = fs::temp_directory_path() /
        ("quorum_test_pitch_" + std::to_string(getpid()) + "_" +
         std::to_string(g_test_num++));
    PitchLayout l;
    l.root = root.string();
    l.vault_dir = (root / ".quorum" / "vaults" / agent).string();
    fs::create_directories(fs::path(l.vault_dir) / "knowledge");
    fs::create_directories(fs::path(l.vault_dir) / "inbox");

    if (with_pitch) {
        write_file(root / "Pitch" / "00 - Introduction.md",
                   "---\n"
                   "title: Demo — Pitch\n"
                   "updated: 2026-05-29\n"
                   "---\n\n"
                   "# Demo\n\n"
                   "## What we're building\n\n"
                   "- A periodic curator that distills scribe output.\n\n"
                   "## Why it matters\n\n"
                   "- Closes the feedback loop.\n\n"
                   "## Current direction\n\n"
                   "- Provenance-on-Sui is the moat.\n");
        write_file(root / "Pitch" / "01 - Anti-goals.md",
                   "---\n"
                   "title: Anti-goals\n"
                   "updated: 2026-05-29\n"
                   "---\n\n"
                   "# Anti-goals\n\n"
                   "## Anti-goals\n\n"
                   "- Do NOT grant the librarian executor tools. (2026-05-28)\n");
    }
    return l;
}

static void cleanup(const std::string& root) { fs::remove_all(root); }

// --- P1: Pitch present + scribe role → digest injected into user_message ----

static void test_p1_pitch_injected_for_scribe() {
    std::cout << "\n=== P1. Pitch present + scribe → '## Project Pitch' ===\n\n";

    auto l = make_layout("scribe", /*with_pitch=*/true);

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "scribe", l.vault_dir, "turn", "task body P1",
        /*team_roster=*/{}, /*skill_file=*/{},
        /*project_root=*/l.root, /*agent_role=*/"scribe");

    check(split.user_message.find("## Project Pitch") != std::string::npos,
          "P1: '## Project Pitch' header present in user_message");
    check(split.system_prompt.find("## Project Pitch") == std::string::npos,
          "P1: '## Project Pitch' absent from system_prompt");

    // Digested content from each of the three sourced sections.
    check(split.user_message.find("periodic curator that distills") !=
              std::string::npos,
          "P1: 'What we're building' content digested");
    check(split.user_message.find("Provenance-on-Sui is the moat") !=
              std::string::npos,
          "P1: 'Current direction' content digested");
    check(split.user_message.find("grant the librarian executor tools") !=
              std::string::npos,
          "P1: 'Anti-goals' content digested");

    // Labels present.
    check(split.user_message.find("**What we're building:**") !=
              std::string::npos,
          "P1: 'What we're building' label rendered");
    check(split.user_message.find("**Current direction:**") !=
              std::string::npos,
          "P1: 'Current direction' label rendered");
    check(split.user_message.find("**Anti-goals:**") != std::string::npos,
          "P1: 'Anti-goals' label rendered");

    cleanup(l.root);
}

// --- P2: Pitch absent → no '## Project Pitch' section -----------------------

static void test_p2_no_pitch_no_section() {
    std::cout << "\n=== P2. Pitch absent → no '## Project Pitch' section ===\n\n";

    auto l = make_layout("scribe", /*with_pitch=*/false);

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "scribe", l.vault_dir, "turn", "task body P2",
        /*team_roster=*/{}, /*skill_file=*/{},
        /*project_root=*/l.root, /*agent_role=*/"scribe");

    check(split.user_message.find("## Project Pitch") == std::string::npos,
          "P2: no '## Project Pitch' section when Pitch files absent");
    // Sanity: the prompt is otherwise well-formed.
    check(split.user_message.find("# Current Task") != std::string::npos,
          "P2: '# Current Task' still emitted (sanity)");

    cleanup(l.root);
}

// --- P3: non-scribe role + Pitch present → no section (scribe-only scope) ---

static void test_p3_non_scribe_no_section() {
    std::cout << "\n=== P3. non-scribe role + Pitch present → no section ===\n\n";

    auto l = make_layout("doer", /*with_pitch=*/true);

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "doer", l.vault_dir, "turn", "task body P3",
        /*team_roster=*/{}, /*skill_file=*/{},
        /*project_root=*/l.root, /*agent_role=*/"doer");

    check(split.user_message.find("## Project Pitch") == std::string::npos,
          "P3: no '## Project Pitch' for a non-scribe role (scribe-only scope)");

    cleanup(l.root);
}

// --- P4: long section body is capped with a trailing "…" marker -------------

static void test_p4_long_section_truncated() {
    std::cout << "\n=== P4. long section body capped with '…' marker ===\n\n";

    auto root = fs::temp_directory_path() /
        ("quorum_test_pitch_long_" + std::to_string(getpid()) + "_" +
         std::to_string(g_test_num++));
    auto vault_dir = (root / ".quorum" / "vaults" / "scribe").string();
    fs::create_directories(fs::path(vault_dir) / "knowledge");

    // ~1200 chars of body in "What we're building" — well over the ~400 cap.
    std::string big;
    for (int i = 0; i < 120; ++i) big += "alpha bravo charlie delta ";
    write_file(root / "Pitch" / "00 - Introduction.md",
               "# Demo\n\n## What we're building\n\n" + big + "\n");

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "scribe", vault_dir, "turn", "task body P4",
        /*team_roster=*/{}, /*skill_file=*/{},
        /*project_root=*/root.string(), /*agent_role=*/"scribe");

    auto pos = split.user_message.find("## Project Pitch");
    check(pos != std::string::npos, "P4: '## Project Pitch' present");

    // The digest line for "What we're building" must be capped: it should
    // contain the truncation marker and be far shorter than the source body.
    check(split.user_message.find(" …") != std::string::npos,
          "P4: truncation marker '…' present (long section capped)");

    // The full 1200-char body must NOT appear verbatim.
    check(split.user_message.find(big) == std::string::npos,
          "P4: full untruncated body NOT present in prompt");

    cleanup(root.string());
}

// --- main -------------------------------------------------------------------

int main() {
    std::cout << "=====================================================\n";
    std::cout << "  Phase 11 Track 4 — ## Project Pitch injection tests\n";
    std::cout << "=====================================================\n";

    test_p1_pitch_injected_for_scribe();
    test_p2_no_pitch_no_section();
    test_p3_non_scribe_no_section();
    test_p4_long_section_truncated();

    std::cout << "\n---------------------------------------------------\n";
    std::cout << "  passed: " << g_passed << "  failed: " << g_failed << "\n";
    std::cout << "---------------------------------------------------\n";

    return g_failed == 0 ? 0 : 1;
}
