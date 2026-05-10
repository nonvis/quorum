// tests/integration/test_inventory_write_side.cpp
// Phase 9 Track 6 #21 + #23 — write-side correctness for inventory-driven
// VAULT_UPDATE flows.
//
// Two scenarios share setup:
//
//   #21 — multi-conversation accumulation.
//     Two conversations, same topic. The second VAULT_UPDATE for
//     `knowledge/rule-foo.md` must OVERWRITE the on-disk file from the first
//     conversation, not create a sibling (no rule-foo-2.md, no duplicate).
//
//   #23 — inventory-aware update wiring (deterministic stub).
//     A pre-existing `knowledge/rule-foo.md` is on disk. A scripted scribe
//     output (no real model call) emits VAULT_UPDATE for the same path. The
//     daemon's parser → vault-write path must land at the SAME file, leaving
//     a single entry in knowledge/. This is the wiring test (prompt-to-write
//     path correctness). The LLM-behavior side lives in Track 9 #33.
//
// No stub-agent class is used: the canonical pattern (see
// test_evaluator_pipeline.cpp::simulate_turn) is to hand-construct the
// agent's "output" as a std::string and feed it through OutputParser →
// VaultManager::apply_all_updates_with_context().
//
// Run:  cd build && ctest -R test_inventory_write_side --output-on-failure

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <sqlite3.h>
#include <unistd.h>

#include "agent/context_assembler.h"
#include "agent/output_parser.h"
#include "daemon/conversation.h"
#include "storage/database.h"
#include "utils/config.h"
#include "vault/vault_manager.h"

// ---- helpers ----------------------------------------------------------------

static int g_passed = 0;
static int g_failed = 0;

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        ++g_failed;
        std::exit(1);
    }
    std::cout << "[PASS] " << msg << "\n";
    ++g_passed;
}

namespace fs = std::filesystem;

// Count regular files in a directory (non-recursive).
static size_t count_files(const fs::path& dir) {
    size_t n = 0;
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return 0;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (entry.is_regular_file()) ++n;
    }
    return n;
}

// Read full file contents.
static std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    if (!in.is_open()) return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
}

// Build the canonical scribe-emitting VAULT_UPDATE block. Uses the multi-line
// `content: |` form so the parser captures the body verbatim (with the 2-space
// indent stripped).
static std::string make_scribe_output(const std::string& path,
                                      const std::string& body) {
    std::string out;
    out += "Update applied.\n\n";
    out += "```VAULT_UPDATE\n";
    out += "path: " + path + "\n";
    out += "content: |\n";
    // Indent each body line with 2 spaces per parser contract.
    size_t start = 0;
    while (start <= body.size()) {
        auto nl = body.find('\n', start);
        if (nl == std::string::npos) {
            out += "  " + body.substr(start) + "\n";
            break;
        }
        out += "  " + body.substr(start, nl - start) + "\n";
        start = nl + 1;
    }
    out += "```\n";
    return out;
}

// Apply a hand-constructed scribe output through the same parser → vault-write
// path used by main.cpp:1037–1048. Returns the count of writes that landed.
static size_t simulate_scribe_turn(sui::quorum::OutputParser& parser,
                                   const sui::quorum::VaultManager& vm,
                                   const std::vector<sui::quorum::AgentMetadata>& team,
                                   const std::string& raw_output) {
    auto parsed = parser.parse(raw_output);
    return vm.apply_all_updates_with_context(
        /*emitting_agent_id=*/"scribe",
        /*emitting_agent_role=*/"scribe",
        /*conversation_mode=*/"generic",
        team,
        parsed.vault_updates);
}

// ---- Case #21 — multi-conversation accumulation -----------------------------

static void test_case_21_multi_conversation_overwrite(const fs::path& tdir) {
    std::cout << "\n=== Case #21 ===\n\n";

    auto base = tdir / "case21";
    fs::create_directories(base);

    sui::quorum::VaultManager vm(base.string());
    check(vm.init_vault("scribe"), "#21: init_vault(scribe) succeeds");

    auto knowledge_dir = base / "vaults" / "scribe" / "knowledge";
    auto rule_path     = knowledge_dir / "rule-foo.md";

    // Roster: a single scribe is enough for generic-mode own-vault writes.
    std::vector<sui::quorum::AgentMetadata> team = {
        sui::quorum::AgentMetadata{
            .id = "scribe", .name = "Scribe",
            .description = "Curates rules", .role = "scribe"
        },
    };

    sui::quorum::OutputParser parser;

    // Conversation 1: write content_v1.
    // Note: the parser's `content: |` form strips trailing newlines on flush
    // (see output_parser.h::parse_kv), so test bodies are authored without a
    // trailing '\n' to keep raw-input == on-disk-content.
    const std::string content_v1 =
        "# rule-foo (v1)\n\nAlways use absolute paths.";
    auto out_v1 = make_scribe_output("knowledge/rule-foo.md", content_v1);
    auto applied_v1 = simulate_scribe_turn(parser, vm, team, out_v1);
    check(applied_v1 == 1, "#21: conversation 1 — 1 update applied");
    check(fs::exists(rule_path),
          "#21: knowledge/rule-foo.md exists after conversation 1");
    check(slurp(rule_path) == content_v1,
          "#21: file content == content_v1 after conversation 1");
    check(count_files(knowledge_dir) == 1,
          "#21: exactly 1 file in knowledge/ after conversation 1");

    // Conversation 2: same path, content_v2. Must overwrite, not create
    // rule-foo-2.md or any sibling.
    const std::string content_v2 =
        "# rule-foo (v2)\n\nAlways use absolute paths AND name files clearly.";
    auto out_v2 = make_scribe_output("knowledge/rule-foo.md", content_v2);
    auto applied_v2 = simulate_scribe_turn(parser, vm, team, out_v2);
    check(applied_v2 == 1, "#21: conversation 2 — 1 update applied");

    // Glob-style check: only one file matching rule-foo*.md.
    size_t rule_foo_matches = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(knowledge_dir, ec)) {
        if (entry.is_regular_file()) {
            auto name = entry.path().filename().string();
            if (name.rfind("rule-foo", 0) == 0 && name.size() > 3 &&
                name.substr(name.size() - 3) == ".md") {
                ++rule_foo_matches;
            }
        }
    }
    check(rule_foo_matches == 1,
          "#21: exactly 1 file matching rule-foo*.md (no sibling)");

    check(slurp(rule_path) == content_v2,
          "#21: file content == content_v2 (overwrite, not append)");
    check(count_files(knowledge_dir) == 1,
          "#21: directory listing of knowledge/ has exactly 1 entry");
}

// ---- Case #23 — inventory-aware update wiring -------------------------------

static void test_case_23_inventory_aware_wiring(const fs::path& tdir) {
    std::cout << "\n=== Case #23 ===\n\n";

    auto base = tdir / "case23";
    fs::create_directories(base);

    sui::quorum::VaultManager vm(base.string());
    check(vm.init_vault("scribe"), "#23: init_vault(scribe) succeeds");
    check(vm.init_vault("leader"), "#23: init_vault(leader) succeeds");

    auto knowledge_dir = base / "vaults" / "scribe" / "knowledge";
    auto rule_path     = knowledge_dir / "rule-foo.md";

    // Pre-existing on-disk content_v1 — the inventory will surface this file.
    const std::string content_v1 =
        "# rule-foo (preexisting)\n\nLegacy rule body.";
    {
        std::ofstream out(rule_path, std::ios::trunc);
        out << content_v1;
    }
    check(fs::exists(rule_path), "#23: preexisting rule-foo.md placed on disk");
    check(count_files(knowledge_dir) == 1,
          "#23: knowledge/ starts with exactly 1 file");

    // 2-agent roster: leader + scribe. mode="generic" satisfies #23.
    std::vector<sui::quorum::AgentMetadata> team = {
        sui::quorum::AgentMetadata{
            .id = "leader", .name = "Leader",
            .description = "Coordinates", .role = "leader"
        },
        sui::quorum::AgentMetadata{
            .id = "scribe", .name = "Scribe",
            .description = "Curates rules", .role = "scribe"
        },
    };

    // Optional sanity-check: the assembler must surface rule-foo.md under
    // `## Vault Inventory`. This proves end-to-end inventory wiring is intact;
    // the load-bearing assertion remains the overwrite check below.
    {
        sui::quorum::ContextAssembler assembler;
        auto split = assembler.assemble_split(
            /*agent_name=*/"scribe",
            /*vault_dir=*/(base / "vaults" / "scribe").string(),
            /*task_type=*/"turn",
            /*task_description=*/"update rule-foo");
        auto inv_pos = split.user_message.find("## Vault Inventory");
        check(inv_pos != std::string::npos,
              "#23: '## Vault Inventory' header present in user_message");
        auto inv_body = split.user_message.substr(inv_pos);
        check(inv_body.find("rule-foo.md") != std::string::npos,
              "#23: rule-foo.md surfaced under Vault Inventory");
    }

    // Drive a scripted scribe turn that emits VAULT_UPDATE for the inventory-
    // listed path. Same simulate_turn pattern as test_evaluator_pipeline.cpp.
    sui::quorum::OutputParser parser;
    const std::string content_v2 =
        "# rule-foo (v2)\n\nUpdated rule body — wiring test.";
    auto raw_output = make_scribe_output("knowledge/rule-foo.md", content_v2);
    auto applied = simulate_scribe_turn(parser, vm, team, raw_output);
    check(applied == 1, "#23: scribe VAULT_UPDATE applied (1 write)");

    // Assertions: same on-disk path, single file, content updated.
    check(fs::exists(rule_path),
          "#23: rule-foo.md still exists at the same on-disk path");
    check(count_files(knowledge_dir) == 1,
          "#23: file count under knowledge/ stayed at 1");
    check(slurp(rule_path) == content_v2,
          "#23: file content == content_v2 (overwritten, not duplicated)");
}

// ---- main -------------------------------------------------------------------

int main() {
    std::cout << "=== Phase 9 Track 6 — Inventory Write-Side Integration Tests ===\n";

    auto tdir = fs::temp_directory_path() /
        ("quorum_test_inv_writeside_" + std::to_string(::getpid()));
    fs::remove_all(tdir);
    fs::create_directories(tdir);

    test_case_21_multi_conversation_overwrite(tdir);
    test_case_23_inventory_aware_wiring(tdir);

    fs::remove_all(tdir);

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";
    return g_failed > 0 ? 1 : 0;
}
