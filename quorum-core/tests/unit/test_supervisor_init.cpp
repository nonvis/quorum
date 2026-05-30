// tests/unit/test_supervisor_init.cpp
// Phase 13 Track 3 — `quorum supervisor init` config generator.
//
// DETERMINISTIC generator (no live claude). Tests the PURE generators
// (generate_supervisor_md + render_checkpoint_skeleton) directly, plus the
// run_supervisor_init bootstrap discipline (foolproof + idempotent).
//
// Assertions:
//   (A) generate_supervisor_md (roster auto-filled):
//       (a) frontmatter + Project + Roster + Record-keeping + Stop conditions +
//           Flight plan headings present
//       (b) seeded agents foo/bar appear as roster rows, with foo's role
//           (thinker) and skill_file rendered
//       (c) the scribe parity CLI command appears; the generated md does NOT
//           contain `quorum librarian curate` (curation is manual, not autopilot)
//       (d) the Flight plan placeholder task (### Task 1) appears
//   (B) generate_supervisor_md (EMPTY roster): the "(no agents configured ...)"
//       line appears and NO agent rows are emitted
//   (C) render_checkpoint_skeleton: heading + Created at: + Major tasks +
//           Morning review present (schema byte-match)
//   (D) run_supervisor_init:
//       (a) creates SUPERVISOR.md + .quorum/autopilot/checkpoint.md
//       (b) a SECOND call WITHOUT force does NOT overwrite SUPERVISOR.md
//       (c) a call WITH force=true regenerates SUPERVISOR.md
//       (d) errors (returns 1) when .quorum/ is absent
//
// Run:  cd build && ctest -R test_supervisor_init --output-on-failure
// Standalone:
//   g++ -std=c++20 -I quorum-core/src \
//     quorum-core/tests/unit/test_supervisor_init.cpp -o /tmp/t && /tmp/t

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <unistd.h>

#include "cli/supervisor_init.h"

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

static std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// ---- Case A: generate_supervisor_md (roster auto-filled) -------------------
static void test_A_generate_with_roster(const fs::path& tdir) {
    std::cout << "\n=== Case A: generate_supervisor_md (with roster) ===\n\n";

    auto proj = tdir / "A_roster";
    fs::create_directories(proj / ".quorum");
    // foo: full agent with role + skill_file.
    write_file(proj / ".quorum" / "agents" / "foo.yaml",
               "id: foo\n"
               "name: \"foo\"\n"
               "role: thinker\n"
               "skill_file: ~/.claude/skills/cartographer/SKILL.md\n");
    // bar: role only (skill_file absent -> em-dash).
    write_file(proj / ".quorum" / "agents" / "bar.yaml",
               "id: bar\n"
               "role: doer\n");

    auto md = sui::quorum::cli::generate_supervisor_md(proj.string());

    // (a) structural headings.
    check(contains(md, "title: Autopilot flight plan"),
          "A(a): frontmatter title present");
    check(contains(md, "generated_by: quorum supervisor init"),
          "A(a): frontmatter generated_by present");
    check(contains(md, "spec_version: 0.2"),
          "A(a): frontmatter spec_version present");
    check(contains(md, "## Project"), "A(a): Project heading present");
    check(contains(md, "- name: A_roster"),
          "A(a): project name = basename of root");
    check(contains(md, "## Roster (subagent workers)"),
          "A(a): Roster heading present");
    check(contains(md, "## Record-keeping (scribe — OUTPUT PARITY, do not bypass)"),
          "A(a): Record-keeping heading present");
    check(contains(md, "## Stop conditions"),
          "A(a): Stop conditions heading present");
    check(contains(md, "## Flight plan"),
          "A(a): Flight plan heading present");

    // (b) roster rows for foo + bar, with foo's role + skill.
    check(contains(md, "| agent | role | skill |"),
          "A(b): roster table header present");
    check(contains(md, "| foo |"), "A(b): foo row present");
    check(contains(md, "| bar |"), "A(b): bar row present");
    check(contains(md, "thinker"), "A(b): foo's role (thinker) rendered");
    check(contains(md, "doer"), "A(b): bar's role (doer) rendered");
    check(contains(md, "~/.claude/skills/cartographer/SKILL.md"),
          "A(b): foo's skill_file rendered");
    // bar has no skill_file -> em-dash in its row.
    check(contains(md, "| bar | doer | \xe2\x80\x94 |"),
          "A(b): bar's empty skill rendered as em-dash");

    // (c) scribe parity command present; librarian curate is NOT emitted
    //     (curation is a manual operator action, never run by autopilot).
    check(contains(md, "quorum scribe record --project " + proj.string()),
          "A(c): scribe record parity command present");
    check(!contains(md, "quorum librarian curate"),
          "A(c): autopilot does NOT auto-run librarian curate (manual only)");
    check(contains(md, "Curation is NOT run by autopilot"),
          "A(c): generated md states curation is manual/out-of-band");

    // (d) flight-plan placeholder.
    check(contains(md, "### Task 1: <replace with your first major task>"),
          "A(d): placeholder Task 1 present");
    check(contains(md, "- agent: <pick a roster agent>"),
          "A(d): placeholder agent line present");
    check(contains(md, "- slices (parallel):"),
          "A(d): placeholder slices line present");
    check(contains(md, "- done when: <criteria>"),
          "A(d): placeholder done-when line present");
}

// ---- Case B: generate_supervisor_md (empty roster) ------------------------
static void test_B_generate_empty_roster(const fs::path& tdir) {
    std::cout << "\n=== Case B: generate_supervisor_md (empty roster) ===\n\n";

    auto proj = tdir / "B_empty_roster";
    fs::create_directories(proj / ".quorum");  // .quorum but no agents/

    auto md = sui::quorum::cli::generate_supervisor_md(proj.string());

    check(contains(md,
                   "(no agents configured — run `quorum agent create` first)"),
          "B: empty-roster line present");
    // No agent rows: the table body line "| <name> |" pattern must be absent.
    // (The header "| agent |" is allowed; assert no data row leaked in.)
    check(!contains(md, "| foo |") && !contains(md, "| bar |"),
          "B: no agent rows emitted on empty roster");
    // The flight-plan placeholder still present (gate stops on it).
    check(contains(md, "### Task 1: <replace with your first major task>"),
          "B: placeholder task still present on empty roster");
}

// ---- Case C: render_checkpoint_skeleton -----------------------------------
static void test_C_checkpoint_skeleton() {
    std::cout << "\n=== Case C: render_checkpoint_skeleton ===\n\n";

    const std::string utc = "2026-05-30T00:00:00Z";
    auto cp = sui::quorum::cli::render_checkpoint_skeleton(utc);

    check(contains(cp, "# Autopilot checkpoint"),
          "C: checkpoint title present");
    check(contains(cp, "Created at: 2026-05-30T00:00:00Z"),
          "C: Created at: stamped with utc");
    check(contains(cp, "Updated at: 2026-05-30T00:00:00Z"),
          "C: Updated at: stamped with utc");
    check(contains(cp, "Flight spec: 0.2"), "C: Flight spec present");
    check(contains(cp, "## Major tasks"), "C: Major tasks heading present");
    check(contains(cp, "## Condensed outcomes"),
          "C: Condensed outcomes heading present");
    check(contains(cp, "## Morning review"),
          "C: Morning review heading present");
    check(contains(cp, "- done: none yet"), "C: morning-review done line");
    check(contains(cp, "- pending: (populated on first run)"),
          "C: morning-review pending line");
    check(contains(cp, "- blocked-on: none"),
          "C: morning-review blocked-on line");
}

// ---- Case D: run_supervisor_init (bootstrap discipline) -------------------
static void test_D_run(const fs::path& tdir) {
    std::cout << "\n=== Case D: run_supervisor_init ===\n\n";

    // (d) absent .quorum/ -> error.
    {
        auto proj = tdir / "D_no_quorum";
        fs::create_directories(proj);
        sui::quorum::cli::SupervisorInitOptions opts;
        opts.project_path = proj.string();
        int rc = sui::quorum::cli::run_supervisor_init(opts);
        check(rc == 1, "D(d): returns 1 when .quorum/ absent");
        check(!fs::exists(proj / "SUPERVISOR.md"),
              "D(d): no SUPERVISOR.md written when .quorum/ absent");
    }

    auto proj = tdir / "D_run";
    fs::create_directories(proj / ".quorum");
    // Seed an agent so the roster is non-trivial.
    write_file(proj / ".quorum" / "agents" / "foo.yaml",
               "id: foo\nrole: thinker\n");

    auto supervisor_path = proj / "SUPERVISOR.md";
    auto checkpoint_path = proj / ".quorum" / "autopilot" / "checkpoint.md";

    // (a) first run creates both artifacts.
    {
        sui::quorum::cli::SupervisorInitOptions opts;
        opts.project_path = proj.string();
        int rc = sui::quorum::cli::run_supervisor_init(opts);
        check(rc == 0, "D(a): first run returns 0");
        check(fs::exists(supervisor_path),
              "D(a): SUPERVISOR.md created");
        check(fs::exists(checkpoint_path),
              "D(a): .quorum/autopilot/checkpoint.md created");
    }

    // (b) second run WITHOUT force does NOT overwrite SUPERVISOR.md.
    {
        std::string before = read_file(supervisor_path);
        // Mutate the file on disk to prove the no-op leaves the operator's edit.
        write_file(supervisor_path, before + "\nOPERATOR_EDIT_MARKER\n");
        std::string edited = read_file(supervisor_path);

        sui::quorum::cli::SupervisorInitOptions opts;
        opts.project_path = proj.string();
        opts.force = false;
        int rc = sui::quorum::cli::run_supervisor_init(opts);
        check(rc == 0, "D(b): second run (no force) returns 0");

        std::string after = read_file(supervisor_path);
        check(after == edited,
              "D(b): SUPERVISOR.md unchanged (operator edit preserved)");
        check(contains(after, "OPERATOR_EDIT_MARKER"),
              "D(b): operator edit marker still present");
    }

    // (c) force=true regenerates SUPERVISOR.md (drops the operator edit).
    {
        sui::quorum::cli::SupervisorInitOptions opts;
        opts.project_path = proj.string();
        opts.force = true;
        int rc = sui::quorum::cli::run_supervisor_init(opts);
        check(rc == 0, "D(c): force run returns 0");

        std::string after = read_file(supervisor_path);
        check(!contains(after, "OPERATOR_EDIT_MARKER"),
              "D(c): force regenerated the file (edit gone)");
        check(contains(after, "# SUPERVISOR.md — Autopilot Flight Plan"),
              "D(c): regenerated content has the canonical title");
    }

    // (e) checkpoint is NEVER clobbered (resume state preserved across reruns).
    {
        std::string before = read_file(checkpoint_path);
        write_file(checkpoint_path, before + "\nRESUME_STATE_MARKER\n");
        std::string edited = read_file(checkpoint_path);

        sui::quorum::cli::SupervisorInitOptions opts;
        opts.project_path = proj.string();
        opts.force = true;  // even force must NOT clobber the checkpoint
        int rc = sui::quorum::cli::run_supervisor_init(opts);
        check(rc == 0, "D(e): rerun returns 0");

        std::string after = read_file(checkpoint_path);
        check(after == edited,
              "D(e): checkpoint.md preserved (resume state not clobbered)");
        check(contains(after, "RESUME_STATE_MARKER"),
              "D(e): resume-state marker still present");
    }
}

// ---- main ------------------------------------------------------------------
int main() {
    auto tdir = fs::path("/tmp") /
                ("quorum_test_supervisor_init_" + std::to_string(::getpid()));
    fs::create_directories(tdir);

    test_A_generate_with_roster(tdir);
    test_B_generate_empty_roster(tdir);
    test_C_checkpoint_skeleton();
    test_D_run(tdir);

    std::error_code ec;
    fs::remove_all(tdir, ec);

    std::cout << "\nResults: " << g_passed << " passed, " << g_failed
              << " failed\n";
    if (g_failed == 0) {
        std::cout << "All supervisor_init tests passed\n";
    }
    return g_failed == 0 ? 0 : 1;
}
