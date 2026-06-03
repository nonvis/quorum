// tests/unit/test_vault_update_brainstorm.cpp
//
// Phase 6 Track 3 / Phase 14 — VAULT_UPDATE path classification.
//
// Phase 14 retired the scribe role and its brainstorm-mode cross-vault
// exception. The rule is now uniform: a VAULT_UPDATE path must be own-shape
// (starts with knowledge/ or inbox/) and lands in the EMITTING agent's vault,
// in BOTH modes for EVERY role. Any cross-vault (`<agent-id>/...`) path is
// rejected. This is the accumulation doctrine "participating knowers self-write
// their OWN lens's slice" — no cross-vault curator.
//
// Tests the pure VaultManager::classify_vault_path() function (no I/O) plus a
// small filesystem integration check that an own-vault write lands and a
// cross-vault write is a silent skip.
//
// Run:  cd build && ctest -R test_vault_update_brainstorm --output-on-failure

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "vault/vault_manager.h"

namespace fs = std::filesystem;
using sui::quorum::AgentMetadata;
using sui::quorum::VaultManager;
using sui::quorum::VaultUpdate;
using sui::quorum::VaultPathClassification;

static int g_failures = 0;

static void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        ++g_failures;
    } else {
        std::cout << "[PASS] " << msg << "\n";
    }
}

// A stable team of knowers. The classifier no longer consults this roster for
// cross-vault prefixes (cross-vault is rejected outright), but the param is
// still part of the signature.
static std::vector<AgentMetadata> make_team() {
    std::vector<AgentMetadata> team;
    AgentMetadata thinker;   thinker.id   = "thinker";   thinker.role   = "thinker";
    AgentMetadata historian; historian.id = "historian"; historian.role = "thinker";
    AgentMetadata doer;      doer.id      = "doer";      doer.role      = "doer";
    team.push_back(thinker);
    team.push_back(historian);
    team.push_back(doer);
    return team;
}

// --- Case A: generic + own-vault path -> ACCEPTED ----------------------------
static void test_A_generic_own_vault() {
    auto team = make_team();
    auto c = VaultManager::classify_vault_path(
        "knowledge/foo.md", "historian", "thinker", "generic", team);

    check(c.accepted,                   "[A] accepted");
    check(!c.is_cross_vault,            "[A] not cross-vault");
    check(c.target_agent == "historian","[A] target = emitting agent");
    check(c.relative_path == "knowledge/foo.md", "[A] relative path preserved");
}

// --- Case B: brainstorm + own-vault path -> ACCEPTED -------------------------
//
// The accumulation doctrine: a participating knower self-writes its OWN lens's
// slice in brainstorm. Own-vault writes work identically in brainstorm.
static void test_B_brainstorm_own_vault() {
    auto team = make_team();
    auto c = VaultManager::classify_vault_path(
        "knowledge/ref-decisions.md", "historian", "thinker", "brainstorm",
        team);

    check(c.accepted,                   "[B] accepted (knower self-write)");
    check(!c.is_cross_vault,            "[B] not cross-vault");
    check(c.target_agent == "historian","[B] target = emitting knower");
    check(c.relative_path == "knowledge/ref-decisions.md",
          "[B] path preserved");
}

// --- Case C: brainstorm + cross-vault path -> REJECTED -----------------------
//
// Phase 14: cross-vault writes are rejected for every role in every mode (the
// scribe exception is gone). A path like "thinker/knowledge/foo.md" is NOT
// own-shape, so it is rejected.
static void test_C_brainstorm_cross_vault_rejected() {
    auto team = make_team();
    auto c = VaultManager::classify_vault_path(
        "thinker/knowledge/insight.md", "historian", "thinker", "brainstorm",
        team);

    check(!c.accepted,        "[C] rejected (cross-vault not permitted)");
    check(!c.reason.empty(),  "[C] rejection has a reason");
    check(c.reason.find("own-vault only") != std::string::npos,
          "[C] reason cites own-vault-only rule");
}

// --- Case D: generic + cross-vault path -> REJECTED --------------------------
static void test_D_generic_cross_vault_rejected() {
    auto team = make_team();
    auto c = VaultManager::classify_vault_path(
        "thinker/knowledge/foo.md", "historian", "thinker", "generic", team);

    check(!c.accepted,       "[D] rejected");
    check(!c.reason.empty(), "[D] rejection has a reason");
}

// --- Case E: doer + cross-vault path in brainstorm -> REJECTED ---------------
static void test_E_brainstorm_doer_cross_vault_rejected() {
    auto team = make_team();
    auto c = VaultManager::classify_vault_path(
        "thinker/knowledge/foo.md", "doer", "doer", "brainstorm", team);

    check(!c.accepted, "[E] rejected (cross-vault not permitted for any role)");
}

// --- Case F: unsafe traversal path -> REJECTED -------------------------------
static void test_F_traversal_rejected() {
    auto team = make_team();
    auto c = VaultManager::classify_vault_path(
        "knowledge/../../etc/passwd", "historian", "thinker", "generic", team);

    check(!c.accepted, "[F] rejected (traversal)");
    check(c.reason.find("unsafe") != std::string::npos,
          "[F] reason cites unsafe path");
}

// --- Filesystem integration: own-vault write lands ---------------------------
static void test_G_own_vault_write_filesystem() {
    auto base = fs::temp_directory_path() / "quorum_p14_vault_own_test";
    std::error_code ec;
    fs::remove_all(base, ec);

    VaultManager vm(base.string());
    check(vm.init_vault("historian"), "[G] init_vault historian");

    auto team = make_team();
    VaultUpdate u;
    u.path    = "knowledge/ref-decisions.md";
    u.content = "knower-self-written decisions survey\n";

    bool ok = vm.apply_vault_update_with_context(
        /*emitting=*/"historian", /*role=*/"thinker",
        /*mode=*/"brainstorm", team, u);
    check(ok, "[G] apply_vault_update_with_context returned true");

    auto own = base / "vaults" / "historian" / "knowledge" / "ref-decisions.md";
    check(fs::exists(own), "[G] own-vault write landed in historian's vault");

    if (fs::exists(own)) {
        std::ifstream in(own);
        std::string body{std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>()};
        check(body == "knower-self-written decisions survey\n",
              "[G] own-vault write contents match");
    }

    fs::remove_all(base, ec);
}

// --- Filesystem integration: cross-vault write is a silent skip --------------
static void test_H_cross_vault_is_silent_skip() {
    auto base = fs::temp_directory_path() / "quorum_p14_vault_reject_test";
    std::error_code ec;
    fs::remove_all(base, ec);

    VaultManager vm(base.string());
    check(vm.init_vault("historian"), "[H] init_vault historian");
    check(vm.init_vault("thinker"),   "[H] init_vault thinker");

    auto team = make_team();
    VaultUpdate u;
    u.path    = "thinker/knowledge/blocked.md";
    u.content = "should never land\n";

    bool ok = vm.apply_vault_update_with_context(
        /*emitting=*/"historian", /*role=*/"thinker",
        /*mode=*/"brainstorm", team, u);
    check(!ok, "[H] returned false (rejected)");

    auto cross = base / "vaults" / "thinker" / "knowledge" / "blocked.md";
    check(!fs::exists(cross),
          "[H] no file created in another agent's vault");

    fs::remove_all(base, ec);
}

// --- main --------------------------------------------------------------------

int main() {
    std::cout << "=== test_vault_update_brainstorm (Phase 14: own-vault only) ===\n\n";

    test_A_generic_own_vault();
    test_B_brainstorm_own_vault();
    test_C_brainstorm_cross_vault_rejected();
    test_D_generic_cross_vault_rejected();
    test_E_brainstorm_doer_cross_vault_rejected();
    test_F_traversal_rejected();
    test_G_own_vault_write_filesystem();
    test_H_cross_vault_is_silent_skip();

    if (g_failures > 0) {
        std::cerr << "\n" << g_failures << " test(s) FAILED.\n";
        return 1;
    }
    std::cout << "\nAll tests passed.\n";
    return 0;
}
