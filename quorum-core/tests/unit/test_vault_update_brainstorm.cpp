// tests/unit/test_vault_update_brainstorm.cpp
//
// Phase 6 Track 3 — VAULT_UPDATE path classification with the scribe
// brainstorm-mode cross-vault exception.
//
// Tests the pure VaultManager::classify_vault_path() function (no I/O)
// for six (mode × role × path-shape) combinations, and a small filesystem
// integration check confirming a brainstorm-scribe cross-write actually
// lands in the OTHER agent's vault.
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

// Build a stable team: thinker, scribe, doer. The classifier consults this
// roster ONLY to validate the cross-vault agent-id prefix; role lookup for
// the emitting agent is supplied separately.
static std::vector<AgentMetadata> make_team() {
    std::vector<AgentMetadata> team;
    AgentMetadata thinker; thinker.id = "thinker"; thinker.role = "thinker";
    AgentMetadata scribe;  scribe.id  = "scribe";  scribe.role  = "scribe";
    AgentMetadata doer;    doer.id    = "doer";    doer.role    = "doer";
    team.push_back(thinker);
    team.push_back(scribe);
    team.push_back(doer);
    return team;
}

// --- Case A: generic + scribe + own-vault path -> ACCEPTED -------------------
//
// Pre-Phase-6 baseline. The scribe in generic mode writing to its own
// knowledge/ folder is the most common path; it must keep working.
static void test_A_generic_scribe_own_vault() {
    auto team = make_team();
    auto c = VaultManager::classify_vault_path(
        "knowledge/foo.md", "scribe", "scribe", "generic", team);

    check(c.accepted,                "[A] accepted");
    check(!c.is_cross_vault,         "[A] not cross-vault");
    check(c.target_agent == "scribe","[A] target = emitting agent (scribe)");
    check(c.relative_path == "knowledge/foo.md", "[A] relative path preserved");
}

// --- Case B: generic + scribe + cross-vault path -> REJECTED -----------------
//
// The exception is brainstorm-only. In generic mode even the scribe is
// own-vault-bound; a path like "thinker/knowledge/foo.md" must be rejected.
static void test_B_generic_scribe_cross_vault_rejected() {
    auto team = make_team();
    auto c = VaultManager::classify_vault_path(
        "thinker/knowledge/foo.md", "scribe", "scribe", "generic", team);

    check(!c.accepted,                "[B] rejected");
    check(!c.reason.empty(),          "[B] rejection has reason");
    check(c.reason.find("generic") != std::string::npos,
          "[B] reason mentions generic mode");
}

// --- Case C: brainstorm + scribe + cross-vault path -> ACCEPTED --------------
//
// The Track 3 invariant: the scribe in brainstorm CAN cross-write into
// another team member's vault. Routing must redirect both target_agent
// and relative_path so the downstream writer drops the file in the right
// place.
static void test_C_brainstorm_scribe_cross_vault_accepted() {
    auto team = make_team();
    auto c = VaultManager::classify_vault_path(
        "thinker/knowledge/insight.md", "scribe", "scribe", "brainstorm", team);

    check(c.accepted,                  "[C] accepted (scribe exception)");
    check(c.is_cross_vault,            "[C] flagged as cross-vault");
    check(c.target_agent == "thinker", "[C] target rerouted to thinker");
    check(c.relative_path == "knowledge/insight.md",
          "[C] agent-id prefix stripped from relative path");
}

// --- Case D: brainstorm + scribe + unknown agent-id -> REJECTED --------------
//
// Cross-write target must be a known team member. Unknown IDs are
// rejected (caller logs and skips; conversation continues).
static void test_D_brainstorm_scribe_unknown_agent_rejected() {
    auto team = make_team();
    auto c = VaultManager::classify_vault_path(
        "ghost/knowledge/foo.md", "scribe", "scribe", "brainstorm", team);

    check(!c.accepted,        "[D] rejected (unknown agent)");
    check(c.reason.find("ghost") != std::string::npos,
          "[D] reason names the unknown agent id");
}

// --- Case E: brainstorm + doer + cross-vault path -> REJECTED ----------------
//
// The exception is scribe-only. A doer in brainstorm mode emitting a
// cross-vault path must be rejected — even though the conversation IS in
// brainstorm. Role gates the exception, not mode alone.
static void test_E_brainstorm_doer_cross_vault_rejected() {
    auto team = make_team();
    auto c = VaultManager::classify_vault_path(
        "thinker/knowledge/foo.md", "doer", "doer", "brainstorm", team);

    check(!c.accepted,                  "[E] rejected (cross-vault is scribe-only)");
    check(c.reason.find("scribe-only") != std::string::npos,
          "[E] reason cites scribe-only restriction");
}

// --- Case F: brainstorm + scribe + own-vault path -> ACCEPTED ----------------
//
// Defensive: the brainstorm-mode scribe must STILL be able to write to
// its own vault using the existing own-vault shape. The exception EXTENDS
// the rule, it does not replace it.
static void test_F_brainstorm_scribe_own_vault_still_accepted() {
    auto team = make_team();
    auto c = VaultManager::classify_vault_path(
        "knowledge/own.md", "scribe", "scribe", "brainstorm", team);

    check(c.accepted,                "[F] accepted");
    check(!c.is_cross_vault,         "[F] not flagged cross-vault");
    check(c.target_agent == "scribe","[F] target = emitting scribe");
    check(c.relative_path == "knowledge/own.md", "[F] path preserved");
}

// --- Filesystem integration: brainstorm-scribe cross-write actually lands ----
//
// The pure classifier tests cover the routing logic. This case wires the
// classifier into apply_vault_update_with_context() and verifies the
// resulting file lands at <vault-root>/thinker/knowledge/insight.md, NOT
// at <vault-root>/scribe/thinker/knowledge/insight.md.
static void test_G_brainstorm_scribe_cross_vault_filesystem() {
    auto base = fs::temp_directory_path() / "quorum_p6t3_vault_test";
    std::error_code ec;
    fs::remove_all(base, ec);

    VaultManager vm(base.string());
    check(vm.init_vault("scribe"),  "[G] init_vault scribe");
    check(vm.init_vault("thinker"), "[G] init_vault thinker");

    auto team = make_team();
    VaultUpdate u;
    u.path    = "thinker/knowledge/insight.md";
    u.content = "scribe-curated insight\n";

    bool ok = vm.apply_vault_update_with_context(
        /*emitting=*/"scribe", /*role=*/"scribe",
        /*mode=*/"brainstorm", team, u);
    check(ok, "[G] apply_vault_update_with_context returned true");

    auto cross = base / "vaults" / "thinker" / "knowledge" / "insight.md";
    auto wrong = base / "vaults" / "scribe"  / "thinker" / "knowledge" / "insight.md";

    check(fs::exists(cross), "[G] cross-write landed in thinker's vault");
    check(!fs::exists(wrong),
          "[G] cross-write did NOT mistakenly nest under scribe's vault");

    if (fs::exists(cross)) {
        std::ifstream in(cross);
        std::string body{std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>()};
        check(body == "scribe-curated insight\n",
              "[G] cross-write contents match");
    }

    fs::remove_all(base, ec);
}

// --- Filesystem integration: rejected update is silently skipped -------------
//
// Doer-in-brainstorm emits a cross-vault path. The classifier rejects it;
// apply_vault_update_with_context must return false WITHOUT throwing and
// MUST NOT create any file. (Caller logs; conversation continues.)
static void test_H_rejection_is_silent_skip() {
    auto base = fs::temp_directory_path() / "quorum_p6t3_reject_test";
    std::error_code ec;
    fs::remove_all(base, ec);

    VaultManager vm(base.string());
    check(vm.init_vault("doer"),    "[H] init_vault doer");
    check(vm.init_vault("thinker"), "[H] init_vault thinker");

    auto team = make_team();
    VaultUpdate u;
    u.path    = "thinker/knowledge/blocked.md";
    u.content = "should never land\n";

    bool ok = vm.apply_vault_update_with_context(
        /*emitting=*/"doer", /*role=*/"doer",
        /*mode=*/"brainstorm", team, u);
    check(!ok, "[H] returned false (rejected)");

    auto cross = base / "vaults" / "thinker" / "knowledge" / "blocked.md";
    check(!fs::exists(cross),
          "[H] no file created in thinker's vault");

    fs::remove_all(base, ec);
}

// --- main --------------------------------------------------------------------

int main() {
    std::cout << "=== test_vault_update_brainstorm (Phase 6 Track 3) ===\n\n";

    test_A_generic_scribe_own_vault();
    test_B_generic_scribe_cross_vault_rejected();
    test_C_brainstorm_scribe_cross_vault_accepted();
    test_D_brainstorm_scribe_unknown_agent_rejected();
    test_E_brainstorm_doer_cross_vault_rejected();
    test_F_brainstorm_scribe_own_vault_still_accepted();
    test_G_brainstorm_scribe_cross_vault_filesystem();
    test_H_rejection_is_silent_skip();

    if (g_failures > 0) {
        std::cerr << "\n" << g_failures << " test(s) FAILED.\n";
        return 1;
    }
    std::cout << "\nAll tests passed.\n";
    return 0;
}
