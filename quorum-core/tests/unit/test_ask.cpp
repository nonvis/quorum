// tests/unit/test_ask.cpp
// Phase 12 — `quorum ask` CLI. PURE helpers only (resolve_project_path +
// assemble_manager_prompt). NO live claude call.
//
// Assertions:
//   (A) resolve_project_path:
//       (a) an existing dir containing .quorum/ resolves to itself
//       (b) a dir WITHOUT .quorum/ returns "" with a non-empty err
//       (c) name-convention: with HOME pointed at a temp dir and
//           <tmpHOME>/nonvis/foo/.quorum present, resolving "foo" finds it
//   (B) assemble_manager_prompt (Phase 14: sources the KNOWER VAULTS):
//       (a) the prompt contains the question
//       (b) seeded knower ref-*.md content (a cartographer + historian ref)
//           appears in the prompt, labelled by source vault
//       (c) with NO knower vaults present it still returns a non-empty prompt
//           containing the question (graceful degrade), no crash
//   (C) agent path (PURE):
//       (a) assemble_agent_prompt embeds the skill sentinel + the knowledge
//           sentinel + the question + the agent/role persona
//       (b) list_agent_names returns the seeded agent stem(s), sorted
//       (c) resolving a non-existent agent yaml fails, and the available-agents
//           list is populated with the seeded names
//
// Run:  cd build && ctest -R test_ask --output-on-failure

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "cli/ask.h"

namespace fs = std::filesystem;

static int g_passed = 0;
static int g_failed = 0;

#define check(cond, msg) do {                                         \
    if (cond) { ++g_passed; std::cout << "  PASS: " << msg << "\n"; } \
    else      { ++g_failed; std::cerr << "  FAIL: " << msg << "\n"; } \
} while (0)

static void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::trunc);
    f << content;
}

// ---- Case A: resolve_project_path -----------------------------------------
static void test_A_resolve(const fs::path& tdir) {
    std::cout << "\n=== Case A: resolve_project_path ===\n\n";

    // (a) existing dir WITH .quorum/ resolves to itself.
    auto with_q = tdir / "A_with_quorum";
    fs::create_directories(with_q / ".quorum");
    {
        std::string err;
        auto resolved = sui::quorum::cli::resolve_project_path(with_q.string(),
                                                               err);
        check(!resolved.empty(), "A(a): dir with .quorum/ resolves non-empty");
        check(err.empty(), "A(a): no error on success");
        // Resolved path points at the same directory (compare canonical forms).
        std::error_code ec;
        check(fs::equivalent(resolved, with_q, ec),
              "A(a): resolved path equivalent to the input dir");
    }

    // (b) existing dir WITHOUT .quorum/ -> "" + non-empty err.
    auto no_q = tdir / "A_no_quorum";
    fs::create_directories(no_q);
    {
        std::string err;
        auto resolved = sui::quorum::cli::resolve_project_path(no_q.string(),
                                                               err);
        check(resolved.empty(), "A(b): dir without .quorum/ returns \"\"");
        check(!err.empty(), "A(b): err is non-empty");
        check(err.find("no .quorum/ found") != std::string::npos,
              "A(b): err mentions 'no .quorum/ found'");
    }

    // (c) name-convention: HOME -> temp, <HOME>/nonvis/foo/.quorum present.
    {
        auto fake_home = tdir / "A_home";
        auto foo_quorum = fake_home / "nonvis" / "foo" / ".quorum";
        fs::create_directories(foo_quorum);

        const char* saved_home = std::getenv("HOME");
        std::string saved = saved_home ? saved_home : "";
        setenv("HOME", fake_home.string().c_str(), 1);

        std::string err;
        auto resolved = sui::quorum::cli::resolve_project_path("foo", err);

        // Restore HOME before asserting (so a failure doesn't leak state).
        if (saved_home) setenv("HOME", saved.c_str(), 1);
        else unsetenv("HOME");

        check(!resolved.empty(),
              "A(c): name 'foo' resolves via <HOME>/nonvis/foo");
        check(err.empty(), "A(c): no error for name-convention resolve");
        auto expected = (fake_home / "nonvis" / "foo").string();
        check(resolved == expected,
              "A(c): resolved path == <HOME>/nonvis/foo");
    }

    // (d) projects root: <HOME>/projects/bar/.quorum present resolves "bar".
    {
        auto fake_home = tdir / "A_home_projects";
        fs::create_directories(fake_home / "projects" / "bar" / ".quorum");

        const char* saved_home = std::getenv("HOME");
        std::string saved = saved_home ? saved_home : "";
        setenv("HOME", fake_home.string().c_str(), 1);

        std::string err;
        auto resolved = sui::quorum::cli::resolve_project_path("bar", err);

        if (saved_home) setenv("HOME", saved.c_str(), 1);
        else unsetenv("HOME");

        check(err.empty(), "A(d): no error resolving via <HOME>/projects");
        auto expected = (fake_home / "projects" / "bar").string();
        check(resolved == expected,
              "A(d): name 'bar' resolves via <HOME>/projects/bar");
    }

    // (e) precedence: projects/ wins over nonvis/ when both hold the name.
    {
        auto fake_home = tdir / "A_home_both";
        fs::create_directories(fake_home / "projects" / "baz" / ".quorum");
        fs::create_directories(fake_home / "nonvis" / "baz" / ".quorum");

        const char* saved_home = std::getenv("HOME");
        std::string saved = saved_home ? saved_home : "";
        setenv("HOME", fake_home.string().c_str(), 1);

        std::string err;
        auto resolved = sui::quorum::cli::resolve_project_path("baz", err);

        if (saved_home) setenv("HOME", saved.c_str(), 1);
        else unsetenv("HOME");

        auto expected = (fake_home / "projects" / "baz").string();
        check(resolved == expected,
              "A(e): 'baz' in both roots resolves to <HOME>/projects (first root)");
    }
}

// ---- Case B: assemble_manager_prompt --------------------------------------
static void test_B_assemble(const fs::path& tdir) {
    std::cout << "\n=== Case B: assemble_manager_prompt ===\n\n";

    const std::string question =
        "What is the project's stance on durable independent storage?";

    // (a)+(b): seeded KNOWER VAULTS (Phase 14: the sole accumulators). The
    // manager prompt derives from the knower ref-*.md surveys, NOT a curated
    // librarian layer.
    {
        auto proj = tdir / "B_seeded";
        fs::create_directories(proj / ".quorum");
        // Cartographer survey (the WHERE lens).
        write_file(proj / ".quorum" / "vaults" / "cartographer" / "knowledge" /
                       "ref-project-index.md",
                   "---\nsummary: provenance-on-Sui layout marker\n---\n"
                   "# Project index\n\n"
                   "CARTOGRAPHER_MARKER: the SAL/Walrus seam is at src/sal/.\n");
        // Historian survey (the WHY lens).
        write_file(proj / ".quorum" / "vaults" / "historian" / "knowledge" /
                       "ref-decisions.md",
                   "# Decisions\n\n### 2026-05-19 — Provenance is the moat\n\n"
                   "HISTORIAN_MARKER: durable-storage claim dropped.\n");

        auto prompt = sui::quorum::cli::assemble_manager_prompt(proj.string(),
                                                                question);
        check(!prompt.empty(), "B(a): prompt non-empty");
        check(prompt.find(question) != std::string::npos,
              "B(a): prompt contains the question");
        // Both knower refs are listed in the inventory (by filename + vault).
        check(prompt.find("ref-project-index.md") != std::string::npos,
              "B(b): cartographer ref listed in knower digest");
        check(prompt.find("ref-decisions.md") != std::string::npos,
              "B(b): historian ref listed in knower digest");
        check(prompt.find("vault: cartographer") != std::string::npos,
              "B(b): cartographer source vault labelled");
        check(prompt.find("vault: historian") != std::string::npos,
              "B(b): historian source vault labelled");
        // The cartographer ref carries a `summary:` so its preview is shown;
        // the historian ref has none, so a head excerpt (its marker) appears.
        check(prompt.find("provenance-on-Sui layout marker")
                  != std::string::npos,
              "B(b): cartographer summary preview appears in prompt");
        check(prompt.find("HISTORIAN_MARKER: durable-storage claim dropped.")
                  != std::string::npos,
              "B(b): historian ref head excerpt appears in prompt");
        // Manager persona present.
        check(prompt.find("You are the manager of this project")
                  != std::string::npos,
              "B(b): manager persona present");
        // No curated-layer / learnings vocabulary leaks into the new prompt.
        check(prompt.find("learnings.md") == std::string::npos,
              "B(b): no learnings.md reference (scribe retired)");
        check(prompt.find(".quorum/librarian") == std::string::npos,
              "B(b): no curated librarian layer reference (librarian retired)");
    }

    // (c): NO knower vaults present -> still non-empty, contains question,
    // graceful-degrade marker for the empty knower digest.
    {
        auto proj = tdir / "B_empty";
        fs::create_directories(proj / ".quorum");  // .quorum exists; no vaults

        auto prompt = sui::quorum::cli::assemble_manager_prompt(proj.string(),
                                                                question);
        check(!prompt.empty(), "B(c): prompt non-empty with no knower vaults");
        check(prompt.find(question) != std::string::npos,
              "B(c): prompt still contains the question (graceful degrade)");
        check(prompt.find("no knower-vault ref-*.md notes found")
                  != std::string::npos,
              "B(c): empty knower digest marked gracefully");
    }
}

// ---- Case C: agent path ----------------------------------------------------
static void test_C_agent(const fs::path& tdir) {
    std::cout << "\n=== Case C: agent path ===\n\n";

    const std::string question =
        "Where does this project's component boundary live?";

    auto proj = tdir / "C_agent";
    fs::create_directories(proj / ".quorum");

    // Seed a skill file (relative, project-rooted) with a sentinel.
    const std::string skill_rel = ".quorum/skills/architect/SKILL.md";
    write_file(proj / skill_rel,
               "# Architect skill\n\n"
               "SKILL_SENTINEL: I map how the system is put together.\n");

    // Seed the agent's recorded knowledge with a sentinel.
    const std::string vault_rel = ".quorum/vaults/architect";
    write_file(proj / vault_rel / "knowledge" / "ref-architecture-map.md",
               "# Architecture map\n\n"
               "KNOWLEDGE_SENTINEL: boundary at the SAL/Walrus seam.\n");

    // Seed the agent yaml (flat key: value, some values quoted).
    write_file(proj / ".quorum" / "agents" / "architect.yaml",
               "id: architect\n"
               "name: \"architect\"\n"
               "role: thinker\n"
               "description: \"the how\"\n"
               "vault_path: " + vault_rel + "\n"
               "context_file: " + vault_rel + "/CONTEXT.md\n"
               "skill_file: " + skill_rel + "\n");

    // (a) assemble_agent_prompt embeds both sentinels + the question + persona.
    {
        auto prompt = sui::quorum::cli::assemble_agent_prompt(
            proj.string(), "architect", "thinker", skill_rel, vault_rel,
            question);
        check(!prompt.empty(), "C(a): agent prompt non-empty");
        check(prompt.find(question) != std::string::npos,
              "C(a): agent prompt contains the question");
        check(prompt.find("SKILL_SENTINEL: I map how the system is put together.")
                  != std::string::npos,
              "C(a): skill sentinel appears in prompt");
        check(prompt.find("KNOWLEDGE_SENTINEL: boundary at the SAL/Walrus seam.")
                  != std::string::npos,
              "C(a): knowledge sentinel appears in prompt");
        check(prompt.find("You are the **architect** (thinker)")
                  != std::string::npos,
              "C(a): agent/role persona present");
    }

    // (b) list_agent_names returns the seeded stem(s), sorted.
    {
        // Add a second agent so we can verify sorted order.
        write_file(proj / ".quorum" / "agents" / "cartographer.yaml",
                   "id: cartographer\nrole: thinker\n");
        auto names =
            sui::quorum::cli::detail::list_agent_names(proj.string());
        check(names.size() == 2, "C(b): two agent stems listed");
        check(!names.empty() && names[0] == "architect",
              "C(b): first stem sorted == architect");
        check(names.size() > 1 && names[1] == "cartographer",
              "C(b): second stem sorted == cartographer");
    }

    // (c) a non-existent agent yaml is absent; available list is populated.
    {
        auto missing = proj / ".quorum" / "agents" / "nonexistent.yaml";
        check(!fs::exists(missing),
              "C(c): non-existent agent yaml does not exist");
        auto names =
            sui::quorum::cli::detail::list_agent_names(proj.string());
        check(!names.empty(),
              "C(c): available-agents list populated for the error message");
    }

    // (d) parse_agent_field unquotes values + exact-key match.
    {
        auto yaml = sui::quorum::detail::read_file_text(
            proj / ".quorum" / "agents" / "architect.yaml");
        check(sui::quorum::cli::detail::parse_agent_field(yaml, "role")
                  == "thinker",
              "C(d): parse role == thinker");
        check(sui::quorum::cli::detail::parse_agent_field(yaml, "name")
                  == "architect",
              "C(d): parse name strips surrounding quotes");
        check(sui::quorum::cli::detail::parse_agent_field(yaml, "skill_file")
                  == skill_rel,
              "C(d): parse skill_file == seeded relative path");
    }
}

// ---- main ------------------------------------------------------------------
int main() {
    auto tdir = fs::temp_directory_path() /
                ("quorum-test-ask-" + std::to_string(::getpid()));
    fs::create_directories(tdir);

    test_A_resolve(tdir);
    test_B_assemble(tdir);
    test_C_agent(tdir);

    std::error_code ec;
    fs::remove_all(tdir, ec);

    std::cout << "\nResults: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
