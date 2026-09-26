// tests/unit/test_agent_relink.cpp
// Unit tests for cli/agent_relink.h — `quorum agent relink` rewrites the
// absolute paths another machine left in .quorum/agents/*.yaml into portable
// refs (~/, $QUORUM/, project-relative). Real /tmp dirs stand in for $HOME,
// the Quorum checkout and the project; no claude, no daemon.
//
// Run:  cd build && ctest -R test_agent_relink

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "cli/agent_relink.h"
#include "utils/config.h"

namespace fs = std::filesystem;
using sui::quorum::cli::plan_relink;
using sui::quorum::cli::relink_path_ref;

static int g_passed = 0;

static void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        std::exit(1);
    }
    std::cout << "[PASS] " << msg << "\n";
    ++g_passed;
}

static void write_file(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

struct Fixture {
    fs::path tmp, home, quorum, project;
};

// $HOME with one installed role skill, a checkout with one lens skill, and a
// project named "meridian" whose agents were written on a Mac.
static Fixture make_fixture() {
    Fixture f;
    f.tmp = fs::temp_directory_path() / ("quorum_test_relink_" + std::to_string(getpid()));
    fs::remove_all(f.tmp);
    f.home = f.tmp / "home";
    f.quorum = f.tmp / "home" / "code" / "quorum";
    f.project = f.tmp / "home" / "work" / "meridian";
    write_file(f.home / ".claude/skills/quorum-roles/doer/SKILL.md", "# doer\n");
    write_file(f.quorum / "templates/skills/cartographer/SKILL.md", "# carto\n");
    write_file(f.project / ".claude/skills/local/SKILL.md", "# local\n");

    const auto agents = f.project / ".quorum" / "agents";
    write_file(agents / "cartographer.yaml",
               "id: cartographer\n"
               "name: \"cartographer\"\n"
               "role: thinker\n"
               "description: \"Cartographer: knows the project layout.\"\n"
               "\n"
               "vault_path: .quorum/vaults/cartographer/\n"
               "context_file: .quorum/vaults/cartographer/CONTEXT.md\n"
               "skill_file: /Users/sangsoo/nonvis/quorum/templates/skills/cartographer/SKILL.md\n");
    write_file(agents / "cpp-dev.yaml",
               "id: cpp-dev\n"
               "role: doer\n"
               "skill_file: \"/Users/sangsoo/.claude/skills/quorum-roles/doer/SKILL.md\"\n"
               "\n"
               "executor:\n"
               "  target_dir: /Users/sangsoo/nonvis/meridian\n"
               "  allowed_tools: all\n");
    write_file(agents / "leader.yaml",
               "id: leader\n"
               "role: leader\n"
               "vault_path: .quorum/vaults/leader/\n");
    write_file(agents / "ghost.yaml",
               "id: ghost\n"
               "role: thinker\n"
               "skill_file: /Users/sangsoo/nowhere/SKILL.md\n");
    write_file(agents / "portable.yaml",
               "id: portable\n"
               "role: thinker\n"
               "skill_file: ~/.claude/skills/quorum-roles/doer/SKILL.md\n");
    write_file(agents / "local.yaml",
               "id: local\n"
               "role: doer\n"
               "skill_file: /Users/sangsoo/nonvis/meridian/.claude/skills/local/SKILL.md\n"
               "executor:\n"
               "  target_dir: " + (f.project / "sub").string() + "\n");
    fs::create_directories(f.project / "sub");
    return f;
}

static const sui::quorum::cli::RelinkEdit* find_edit(const sui::quorum::cli::RelinkPlan& plan,
                                                     const std::string& agent,
                                                     const std::string& field) {
    for (const auto& e : plan.edits) {
        if (e.agent == agent && e.field == field) return &e;
    }
    return nullptr;
}

static bool writes(const sui::quorum::cli::RelinkPlan& plan, const std::string& agent) {
    return std::any_of(plan.writes.begin(), plan.writes.end(), [&](const auto& w) {
        return fs::path(w.first).stem() == agent;
    });
}

static std::string written(const sui::quorum::cli::RelinkPlan& plan, const std::string& agent) {
    for (const auto& w : plan.writes) {
        if (fs::path(w.first).stem() == agent) return w.second;
    }
    return {};
}

static void test_plan(const Fixture& f) {
    std::cout << "\n=== A. plan_relink over Mac-written agents ===\n\n";
    auto plan = plan_relink(f.project.string(), f.quorum.string(), f.home.string());

    auto* carto = find_edit(plan, "cartographer", "skill_file");
    check(carto && carto->resolved && carto->to == "$QUORUM/templates/skills/cartographer/SKILL.md",
          "A1: checkout lens skill -> $QUORUM/templates/skills/...");
    auto* doer = find_edit(plan, "cpp-dev", "skill_file");
    check(doer && doer->to == "~/.claude/skills/quorum-roles/doer/SKILL.md",
          "A2: installed role skill -> ~/.claude/skills/...");
    auto* tdir = find_edit(plan, "cpp-dev", "target_dir");
    check(tdir && tdir->to == ".", "A3: target_dir naming the project dir -> .");
    auto* local = find_edit(plan, "local", "skill_file");
    check(local && local->to == ".claude/skills/local/SKILL.md",
          "A4: a skill inside the project -> project-relative");
    auto* local_dir = find_edit(plan, "local", "target_dir");
    check(local_dir && local_dir->to == "sub", "A5: an existing absolute target_dir -> relative");
    check(find_edit(plan, "leader", "skill_file") == nullptr, "A6: no skill_file -> no edit");

    auto* ghost = find_edit(plan, "ghost", "skill_file");
    check(ghost && !ghost->resolved && ghost->to == ghost->from,
          "A7: nothing resolves -> unresolved, value kept");
    auto* port = find_edit(plan, "portable", "skill_file");
    check(port && port->resolved && port->to == port->from, "A8: already portable -> kept");

    check(writes(plan, "cartographer") && writes(plan, "cpp-dev") && writes(plan, "local"),
          "A9: changed files are rewritten");
    check(!writes(plan, "leader") && !writes(plan, "ghost") && !writes(plan, "portable"),
          "A10: unchanged / unresolved files are not rewritten");

    auto cpp = written(plan, "cpp-dev");
    check(contains(cpp, "skill_file: \"~/.claude/skills/quorum-roles/doer/SKILL.md\"\n"),
          "A11: a quoted value stays quoted");
    check(contains(cpp, "\n  target_dir: .\n  allowed_tools: all\n"),
          "A12: indent and the neighbouring executor lines are kept");
    auto carto_text = written(plan, "cartographer");
    check(contains(carto_text, "description: \"Cartographer: knows the project layout.\"\n") &&
              contains(carto_text, "context_file: .quorum/vaults/cartographer/CONTEXT.md\n"),
          "A13: every other line is byte-identical");
    check(!contains(carto_text, "/Users/"), "A14: no Mac path left in the rewritten file");
}

static void test_apply_round_trip(const Fixture& f) {
    std::cout << "\n=== B. the rewritten yaml loads and resolves ===\n\n";
    auto plan = plan_relink(f.project.string(), f.quorum.string(), f.home.string());
    for (const auto& [path, text] : plan.writes) std::ofstream(path) << text;

    auto agent = sui::quorum::load_agent_config(
        (f.project / ".quorum/agents/cpp-dev.yaml").string());
    check(agent.has_value(), "B1: rewritten cpp-dev.yaml parses");
    check(agent->skill_file == "~/.claude/skills/quorum-roles/doer/SKILL.md",
          "B2: the parser reads the portable skill ref");
    check(fs::exists(sui::quorum::expand_path_ref(agent->skill_file, f.project.string(),
                                                  f.quorum.string(), f.home.string())),
          "B3: the ref expands to the installed skill");
    check(sui::quorum::expand_path_ref(agent->target_dir, f.project.string(), f.quorum.string(),
                                       f.home.string()) == f.project.string(),
          "B4: target_dir . expands to the project root");

    auto again = plan_relink(f.project.string(), f.quorum.string(), f.home.string());
    check(again.writes.empty(), "B5: a second relink is a no-op");
}

static void test_relink_path_ref(const Fixture& f) {
    std::cout << "\n=== C. relink_path_ref edge cases ===\n\n";
    const auto P = f.project.string(), Q = f.quorum.string(), H = f.home.string();
    check(relink_path_ref("/Users/x/meridian/", P, Q, H) == std::optional<std::string>("."),
          "C1: trailing slash on a foreign project dir -> .");
    check(!relink_path_ref("/Users/x/other-project", P, Q, H).has_value(),
          "C2: a foreign dir that is not this project -> unresolved");
    check(!relink_path_ref("relative/missing", P, Q, H).has_value(),
          "C3: a missing relative ref -> unresolved");
}

int main() {
    auto f = make_fixture();
    test_plan(f);
    test_apply_round_trip(f);
    test_relink_path_ref(f);
    fs::remove_all(f.tmp);
    std::cout << "\n" << g_passed << " checks passed\n";
    return 0;
}
