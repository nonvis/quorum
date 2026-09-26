// tests/unit/test_path_ref.cpp
// Unit tests for utils/path_ref.h — the portable path references (~/,
// $QUORUM/, project-relative) that tracked .quorum/ files store instead of
// one machine's absolute paths. Pure: no filesystem, no environment.
//
// Run:  cd build && ctest -R test_path_ref

#include <cstdlib>
#include <iostream>
#include <string>

#include "utils/path_ref.h"

using sui::quorum::expand_path_ref;
using sui::quorum::portable_path_ref;

static int g_passed = 0;

static void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        std::exit(1);
    }
    std::cout << "[PASS] " << msg << "\n";
    ++g_passed;
}

static void check_eq(const std::string& got, const std::string& want, const std::string& msg) {
    if (got != want) {
        std::cerr << "[FAIL] " << msg << "\n       got:  " << got << "\n       want: " << want << "\n";
        std::exit(1);
    }
    std::cout << "[PASS] " << msg << "\n";
    ++g_passed;
}

static const std::string H = "/h";
static const std::string Q = "/h/nonvis/quorum";
static const std::string P = "/h/nonvis/meridian";

static void test_expand() {
    std::cout << "\n=== A. expand_path_ref ===\n\n";
    check_eq(expand_path_ref("~/.claude/skills/x/SKILL.md", P, Q, H),
             "/h/.claude/skills/x/SKILL.md", "A1: ~/ -> home");
    check_eq(expand_path_ref("~", P, Q, H), "/h", "A2: bare ~ -> home");
    check_eq(expand_path_ref("$QUORUM/templates/skills/recap/SKILL.md", P, Q, H),
             "/h/nonvis/quorum/templates/skills/recap/SKILL.md", "A3: $QUORUM/ -> quorum root");
    check_eq(expand_path_ref("$QUORUM", P, Q, H), Q, "A4: bare $QUORUM -> quorum root");
    check_eq(expand_path_ref(".", P, Q, H), P, "A5: . -> project root, no trailing slash");
    check_eq(expand_path_ref("sub/dir", P, Q, H), "/h/nonvis/meridian/sub/dir",
             "A6: relative -> project-rooted");
    check_eq(expand_path_ref("./.claude/skills/a/SKILL.md", P, Q, H),
             "/h/nonvis/meridian/.claude/skills/a/SKILL.md", "A7: ./ prefix normalized");
    check_eq(expand_path_ref("/abs/x", P, Q, H), "/abs/x", "A8: absolute unchanged");
    check(expand_path_ref("$QUORUMX/foo", P, Q, H).find(Q) == std::string::npos,
          "A9: $QUORUMX is not the $QUORUM token");
    check(expand_path_ref("~user/x", P, Q, H).find("/h/user") == std::string::npos,
          "A10: ~user is not the ~ token");
    check_eq(expand_path_ref("~/x", P, Q, ""), "~/x", "A11: unknown home -> unchanged");
    check_eq(expand_path_ref("$QUORUM/x", P, "", H), "$QUORUM/x", "A12: unknown quorum root -> unchanged");
    check_eq(expand_path_ref("rel", "", Q, H), "rel", "A13: no project root -> relative unchanged");
    check_eq(expand_path_ref("", P, Q, H), "", "A14: empty -> empty");
}

static void test_portable() {
    std::cout << "\n=== B. portable_path_ref ===\n\n";
    check_eq(portable_path_ref("/h/.claude/skills/quorum-roles/doer/SKILL.md", P, Q, H),
             "~/.claude/skills/quorum-roles/doer/SKILL.md", "B1: under home -> ~/");
    check_eq(portable_path_ref("/h/nonvis/quorum/templates/skills/recap/SKILL.md", P, Q, H),
             "$QUORUM/templates/skills/recap/SKILL.md", "B2: under the checkout -> $QUORUM/ (beats ~)");
    check_eq(portable_path_ref(P, P, Q, H), ".", "B3: the project root itself -> .");
    check_eq(portable_path_ref(P + "/", P, Q, H), ".", "B4: trailing slash -> .");
    check_eq(portable_path_ref(P + "/sub/x", P, Q, H), "sub/x", "B5: inside the project -> relative");
    check_eq(portable_path_ref("/h/nonvis/meridian2/x", P, Q, H), "~/nonvis/meridian2/x",
             "B6: a sibling sharing the name prefix is NOT inside the project");
    check_eq(portable_path_ref("/opt/x/SKILL.md", P, Q, H), "/opt/x/SKILL.md",
             "B7: outside every root -> unchanged");
    check_eq(portable_path_ref("templates/x", P, Q, H), "templates/x", "B8: relative input unchanged");
    check_eq(portable_path_ref("", P, Q, H), "", "B9: empty -> empty");

    // The project nested in the checkout (e.g. quorum/sample): most specific wins.
    const std::string sample = Q + "/sample";
    check_eq(portable_path_ref(sample + "/x", sample, Q, H), "x", "B10: nested project file -> relative");
    check_eq(portable_path_ref(Q + "/templates/y", sample, Q, H), "$QUORUM/templates/y",
             "B11: checkout file outside the nested project -> $QUORUM/");
    // Self-hosted: the project IS the checkout — the tie goes to the project.
    check_eq(portable_path_ref(Q + "/templates/y", Q, Q, H), "templates/y",
             "B12: project == checkout -> project-relative");
    check_eq(portable_path_ref("/h/x", P, Q, ""), "/h/x", "B13: unknown home -> no ~ form");
}

static void test_round_trip() {
    std::cout << "\n=== C. expand(portable(x)) == x ===\n\n";
    for (const std::string abs : {
             std::string("/h/.claude/skills/quorum-roles/leader/SKILL.md"),
             std::string("/h/nonvis/quorum/templates/skills/architect/SKILL.md"),
             P, P + "/sub/dir", std::string("/opt/elsewhere/SKILL.md")}) {
        check_eq(expand_path_ref(portable_path_ref(abs, P, Q, H), P, Q, H), abs,
                 "C: round trip " + abs);
    }
}

int main() {
    test_expand();
    test_portable();
    test_round_trip();
    std::cout << "\n" << g_passed << " checks passed\n";
    return 0;
}
