#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include "vault/vault_manager.h"

namespace fs = std::filesystem;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    std::cout << "  " << #name << "... "; \
    if (test_##name()) { std::cout << "PASS\n"; ++tests_passed; } \
    else { std::cout << "FAIL\n"; ++tests_failed; }

static const std::string TEST_DIR = "/tmp/quorum_vault_test_data";

static void cleanup() {
    std::error_code ec;
    fs::remove_all(TEST_DIR, ec);
}

// ── Tests ───────────────────────────────────────────────────────────────────

static bool test_init_vault() {
    sui::quorum::VaultManager vm(TEST_DIR);

    if (!vm.init_vault("market_analyst")) return false;

    // Verify directories exist
    if (!fs::is_directory(TEST_DIR + "/vaults/market_analyst")) return false;
    if (!fs::is_directory(TEST_DIR + "/vaults/market_analyst/knowledge")) return false;
    if (!fs::is_directory(TEST_DIR + "/vaults/market_analyst/inbox")) return false;

    // Idempotent — calling again should succeed
    if (!vm.init_vault("market_analyst")) return false;

    return true;
}

static bool test_vault_path() {
    sui::quorum::VaultManager vm(TEST_DIR);

    auto path = vm.vault_path("bot_analyst");
    if (path != TEST_DIR + "/vaults/bot_analyst") return false;

    return true;
}

static bool test_exists() {
    sui::quorum::VaultManager vm(TEST_DIR);

    // market_analyst was created in init_vault test
    if (!vm.exists("market_analyst")) return false;

    // nonexistent agent
    if (vm.exists("nonexistent_agent_xyz")) return false;

    return true;
}

static bool test_apply_vault_update() {
    sui::quorum::VaultManager vm(TEST_DIR);
    (void)vm.init_vault("market_analyst");

    sui::quorum::VaultUpdate update;
    update.path = "knowledge/spread-analysis.md";
    update.content = "# Spread Analysis\n\nBTC/USDT spread is narrowing.\nCurrent: 0.02%\n";

    if (!vm.apply_vault_update("market_analyst", update)) return false;

    // Verify file exists
    auto full_path = vm.vault_path("market_analyst") + "/knowledge/spread-analysis.md";
    if (!fs::exists(full_path)) return false;

    // Verify content
    auto content = vm.read_file("market_analyst", "knowledge/spread-analysis.md");
    if (!content) return false;
    if (*content != update.content) return false;

    return true;
}

static bool test_apply_vault_update_nested_dirs() {
    sui::quorum::VaultManager vm(TEST_DIR);
    (void)vm.init_vault("engineer");

    sui::quorum::VaultUpdate update;
    update.path = "knowledge/deep/nested/report.md";
    update.content = "# Deep Report\nNested directory test.\n";

    if (!vm.apply_vault_update("engineer", update)) return false;

    auto content = vm.read_file("engineer", "knowledge/deep/nested/report.md");
    if (!content) return false;
    if (*content != update.content) return false;

    return true;
}

static bool test_apply_vault_update_overwrite() {
    sui::quorum::VaultManager vm(TEST_DIR);

    sui::quorum::VaultUpdate update1;
    update1.path = "knowledge/spread-analysis.md";
    update1.content = "Version 1";
    (void)vm.apply_vault_update("market_analyst", update1);

    sui::quorum::VaultUpdate update2;
    update2.path = "knowledge/spread-analysis.md";
    update2.content = "Version 2 — updated content";
    (void)vm.apply_vault_update("market_analyst", update2);

    auto content = vm.read_file("market_analyst", "knowledge/spread-analysis.md");
    if (!content) return false;
    if (*content != "Version 2 — updated content") return false;

    return true;
}

static bool test_path_traversal_dotdot() {
    sui::quorum::VaultManager vm(TEST_DIR);

    sui::quorum::VaultUpdate bad;
    bad.path = "../../etc/passwd";
    bad.content = "evil";

    // Must reject
    if (vm.apply_vault_update("market_analyst", bad)) return false;
    return true;
}

static bool test_path_traversal_absolute() {
    sui::quorum::VaultManager vm(TEST_DIR);

    sui::quorum::VaultUpdate bad;
    bad.path = "/etc/passwd";
    bad.content = "evil";

    if (vm.apply_vault_update("market_analyst", bad)) return false;
    return true;
}

static bool test_path_traversal_mid_dotdot() {
    sui::quorum::VaultManager vm(TEST_DIR);

    sui::quorum::VaultUpdate bad;
    bad.path = "knowledge/../../../etc/shadow";
    bad.content = "evil";

    if (vm.apply_vault_update("market_analyst", bad)) return false;
    return true;
}

static bool test_path_traversal_just_dotdot() {
    sui::quorum::VaultManager vm(TEST_DIR);

    sui::quorum::VaultUpdate bad;
    bad.path = "..";
    bad.content = "evil";

    if (vm.apply_vault_update("market_analyst", bad)) return false;
    return true;
}

static bool test_path_empty() {
    sui::quorum::VaultManager vm(TEST_DIR);

    sui::quorum::VaultUpdate bad;
    bad.path = "";
    bad.content = "evil";

    if (vm.apply_vault_update("market_analyst", bad)) return false;
    return true;
}

static bool test_read_file_nonexistent() {
    sui::quorum::VaultManager vm(TEST_DIR);

    auto content = vm.read_file("market_analyst", "knowledge/does-not-exist.md");
    if (content.has_value()) return false;

    return true;
}

static bool test_list_files() {
    sui::quorum::VaultManager vm(TEST_DIR);
    (void)vm.init_vault("operator");

    // Write a few files
    (void)vm.apply_vault_update("operator", {"knowledge/charlie.md", "c"});
    (void)vm.apply_vault_update("operator", {"knowledge/alpha.md", "a"});
    (void)vm.apply_vault_update("operator", {"knowledge/bravo.md", "b"});

    auto files = vm.list_files("operator", "knowledge");
    if (files.size() != 3) return false;

    // Should be sorted alphabetically
    if (files[0] != "alpha.md") return false;
    if (files[1] != "bravo.md") return false;
    if (files[2] != "charlie.md") return false;

    return true;
}

static bool test_list_files_empty_dir() {
    sui::quorum::VaultManager vm(TEST_DIR);
    (void)vm.init_vault("bot_analyst");

    auto files = vm.list_files("bot_analyst", "inbox");
    if (!files.empty()) return false;

    return true;
}

static bool test_list_files_nonexistent_dir() {
    sui::quorum::VaultManager vm(TEST_DIR);

    auto files = vm.list_files("market_analyst", "nonexistent");
    if (!files.empty()) return false;

    return true;
}

static bool test_apply_all_updates() {
    sui::quorum::VaultManager vm(TEST_DIR);
    (void)vm.init_vault("bot_analyst");

    std::vector<sui::quorum::VaultUpdate> updates = {
        {"knowledge/pools.md", "# Pool data\n"},
        {"knowledge/metrics.md", "# Metrics\n"},
        {"inbox/task-result.md", "# Task result\n"},
    };

    auto count = vm.apply_all_updates("bot_analyst", updates);
    if (count != 3) return false;

    // Verify all written
    if (!vm.read_file("bot_analyst", "knowledge/pools.md")) return false;
    if (!vm.read_file("bot_analyst", "knowledge/metrics.md")) return false;
    if (!vm.read_file("bot_analyst", "inbox/task-result.md")) return false;

    return true;
}

static bool test_apply_all_updates_partial() {
    sui::quorum::VaultManager vm(TEST_DIR);
    (void)vm.init_vault("bot_analyst");

    std::vector<sui::quorum::VaultUpdate> updates = {
        {"knowledge/good.md", "good content"},
        {"../../etc/evil", "evil content"},   // should fail
        {"knowledge/also-good.md", "also good"},
    };

    auto count = vm.apply_all_updates("bot_analyst", updates);
    if (count != 2) return false;  // 2 succeed, 1 rejected

    return true;
}

// ── Main ────────────────────────────────────────────────────────────────────

int main() {
    cleanup();

    std::cout << "\n=== VaultManager Tests ===\n\n";

    TEST(init_vault);
    TEST(vault_path);
    TEST(exists);
    TEST(apply_vault_update);
    TEST(apply_vault_update_nested_dirs);
    TEST(apply_vault_update_overwrite);
    TEST(path_traversal_dotdot);
    TEST(path_traversal_absolute);
    TEST(path_traversal_mid_dotdot);
    TEST(path_traversal_just_dotdot);
    TEST(path_empty);
    TEST(read_file_nonexistent);
    TEST(list_files);
    TEST(list_files_empty_dir);
    TEST(list_files_nonexistent_dir);
    TEST(apply_all_updates);
    TEST(apply_all_updates_partial);

    std::cout << "\n--- Results: " << tests_passed << " passed, "
              << tests_failed << " failed ---\n\n";

    cleanup();

    return tests_failed > 0 ? 1 : 0;
}
