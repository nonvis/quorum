// tests/unit/test_assembler_inventory.cpp
// Phase 9 Track 1 + Track 3 — ## Vault Inventory section in user_message.
//
// The inventory lists every rule-*.md and ref-*.md in the agent's resolution
// scope (project + role + agent vault, plus teammate vaults for brainstorm-
// mode scribes) so the agent can decide whether a VAULT_UPDATE should target
// an existing file or create a new one.
//
// Track 1 emits filename + scope label + ISO-8601 mtime.
// Track 3 enriches the line format to:
//     `- <name> [t1, t2] (<scope>) — <human mtime>`
// — `[tags]` block is omitted entirely when frontmatter has no tags
// — mtime renders as "just now" / "Nm ago" / "Nh ago" / "Nd ago"
//
// Run:  cd build && ctest -R test_assembler_inventory --output-on-failure

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <thread>

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

static std::string make_temp_vault() {
    auto dir = fs::temp_directory_path() /
        ("quorum_test_inventory_" + std::to_string(getpid()) + "_" +
         std::to_string(g_test_num++));
    fs::create_directories(dir);
    fs::create_directories(dir / "knowledge");
    fs::create_directories(dir / "inbox");
    return dir.string();
}

static void cleanup(const std::string& path) {
    fs::remove_all(path);
}

static void write_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path);
    out << content;
}

static void write_knowledge(const std::string& vault, const std::string& name,
                            const std::string& content) {
    write_file(fs::path(vault) / "knowledge" / name, content);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
}

// Project + role + agent layout (same shape as test_assembler_rule_cap).
struct ScopedLayout {
    std::string root;
    std::string vault_dir;       // <root>/.quorum/vaults/<agent>/
    std::string project_kdir;    // <root>/.quorum/knowledge/
    std::string role_kdir;       // <root>/.quorum/knowledge/roles/<role>/
    std::string vault_kdir;      // <root>/.quorum/vaults/<agent>/knowledge/
};

static ScopedLayout make_scoped_layout(const std::string& agent_name,
                                       const std::string& role) {
    auto root = fs::temp_directory_path() /
        ("quorum_test_inv_scope_" + std::to_string(getpid()) + "_" +
         std::to_string(g_test_num++));
    ScopedLayout l;
    l.root = root.string();
    l.vault_dir = (root / ".quorum" / "vaults" / agent_name).string();
    l.project_kdir = (root / ".quorum" / "knowledge").string();
    l.role_kdir = (root / ".quorum" / "knowledge" / "roles" / role).string();
    l.vault_kdir = (root / ".quorum" / "vaults" / agent_name / "knowledge").string();
    fs::create_directories(l.project_kdir);
    fs::create_directories(l.role_kdir);
    fs::create_directories(l.vault_kdir);
    return l;
}

static void write_kfile(const std::string& kdir, const std::string& name,
                        const std::string& content) {
    write_file(fs::path(kdir) / name, content);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
}

// Count lines in text starting with "- " between the inventory header and
// the next blank-line section break or transparency line.
static size_t count_inventory_entries(const std::string& prompt) {
    auto hdr = prompt.find("## Vault Inventory");
    if (hdr == std::string::npos) return 0;
    // Skip past the header + intro paragraph + blank line.
    auto body_start = prompt.find("\n- ", hdr);
    if (body_start == std::string::npos) return 0;
    size_t count = 0;
    size_t pos = body_start + 1;  // skip leading newline
    while (pos < prompt.size()) {
        if (prompt.compare(pos, 2, "- ") == 0) {
            ++count;
            auto eol = prompt.find('\n', pos);
            if (eol == std::string::npos) break;
            pos = eol + 1;
        } else {
            break;
        }
    }
    return count;
}

// --- T1: basic — 2 rules + 2 refs + 1 plain → only rules + refs in inventory

static void test_t1_basic_inventory() {
    std::cout << "\n=== T1. basic inventory (2 rules + 2 refs + 1 plain) ===\n\n";

    auto vault = make_temp_vault();
    write_knowledge(vault, "rule-alpha.md", "RULE_BODY_ALPHA\n");
    write_knowledge(vault, "rule-beta.md", "RULE_BODY_BETA\n");
    write_knowledge(vault, "ref-foo.md", "REF_BODY_FOO\n");
    write_knowledge(vault, "ref-bar.md", "REF_BODY_BAR\n");
    write_knowledge(vault, "plain-note.md", "PLAIN_BODY\n");

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "agent-t1", vault, "turn", "task body T1");

    check(split.user_message.find("## Vault Inventory") != std::string::npos,
          "T1: '## Vault Inventory' header present in user_message");
    check(split.system_prompt.find("## Vault Inventory") == std::string::npos,
          "T1: '## Vault Inventory' absent from system_prompt");

    // Locate the inventory body to scope filename assertions.
    auto inv_pos = split.user_message.find("## Vault Inventory");
    auto inv_body = split.user_message.substr(inv_pos);
    // Stop at the next "---" or "# Inbox:" or "# Current Task" boundary.
    auto end_a = inv_body.find("---");
    auto end_b = inv_body.find("# Inbox:");
    auto end_c = inv_body.find("# Current Task");
    auto end = std::min({end_a, end_b, end_c});
    if (end != std::string::npos) inv_body = inv_body.substr(0, end);

    check(inv_body.find("rule-alpha.md") != std::string::npos,
          "T1: rule-alpha.md listed in inventory");
    check(inv_body.find("rule-beta.md") != std::string::npos,
          "T1: rule-beta.md listed in inventory");
    check(inv_body.find("ref-foo.md") != std::string::npos,
          "T1: ref-foo.md listed in inventory");
    check(inv_body.find("ref-bar.md") != std::string::npos,
          "T1: ref-bar.md listed in inventory");
    check(inv_body.find("plain-note.md") == std::string::npos,
          "T1: plain-note.md NOT listed in inventory");

    // Track 3: each line should have a human-relative mtime, not ISO.
    // Format: "just now" | "<N>m ago" | "<N>h ago" | "<N>d ago".
    std::regex human_mtime(R"((just now|\d+m ago|\d+h ago|\d+d ago))");
    auto begin = std::sregex_iterator(inv_body.begin(), inv_body.end(),
                                       human_mtime);
    auto end_it = std::sregex_iterator();
    auto human_count = std::distance(begin, end_it);
    check(human_count >= 4,
          "T1: at least 4 human-relative mtimes in inventory body (one per entry)");

    // Track 3: ISO timestamps must NOT appear in the inventory body anymore.
    std::regex iso(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)");
    auto iso_begin = std::sregex_iterator(inv_body.begin(), inv_body.end(), iso);
    auto iso_count = std::distance(iso_begin, end_it);
    check(iso_count == 0,
          "T1: no ISO-8601 timestamps in inventory body (replaced by human mtime)");

    // Track 3: untagged files must not show a `[tags]` bracket pair —
    // the line goes straight from filename to "(<scope>)".
    check(inv_body.find("rule-alpha.md (vault:") != std::string::npos,
          "T1: untagged rule-alpha line goes directly from filename to (vault:...)");

    cleanup(vault);
}

// --- T2: scope coverage — project + role + agent rules + agent ref ----------

static void test_t2_scope_coverage() {
    std::cout << "\n=== T2. scope coverage (project + role + agent + ref) ===\n\n";

    auto layout = make_scoped_layout("agent-t2", "doer");
    write_kfile(layout.project_kdir, "rule-project.md", "PROJECT_RULE_BODY\n");
    write_kfile(layout.role_kdir,    "rule-role.md",    "ROLE_RULE_BODY\n");
    write_kfile(layout.vault_kdir,   "rule-agent.md",   "AGENT_RULE_BODY\n");
    write_kfile(layout.vault_kdir,   "ref-agent.md",    "AGENT_REF_BODY\n");

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "agent-t2", layout.vault_dir, "turn", "task body T2",
        /*team_roster=*/{}, /*skill_file=*/{},
        /*project_root=*/layout.root, /*agent_role=*/"doer");

    auto inv_pos = split.user_message.find("## Vault Inventory");
    check(inv_pos != std::string::npos,
          "T2: inventory header present");
    auto inv_body = split.user_message.substr(inv_pos);
    auto end_a = inv_body.find("---");
    auto end_b = inv_body.find("# Inbox:");
    auto end_c = inv_body.find("# Current Task");
    auto end = std::min({end_a, end_b, end_c});
    if (end != std::string::npos) inv_body = inv_body.substr(0, end);

    // Track 3: untagged files render with no `[tags]` block, so the literal
    // "<filename> (<scope>)" pattern is unchanged from Track 1.
    check(inv_body.find("rule-project.md (project)") != std::string::npos,
          "T2: project rule labeled '(project)' (no tag block when untagged)");
    check(inv_body.find("rule-role.md (role: doer)") != std::string::npos,
          "T2: role rule labeled '(role: doer)' (no tag block when untagged)");
    check(inv_body.find("rule-agent.md (vault: agent-t2)") != std::string::npos,
          "T2: agent rule labeled '(vault: agent-t2)' (no tag block when untagged)");
    check(inv_body.find("ref-agent.md (vault: agent-t2)") != std::string::npos,
          "T2: agent ref labeled '(vault: agent-t2)' (no tag block when untagged)");

    cleanup(layout.root);
}

// --- T3: cap — 60 rule files → 50 listed + transparency line ---------------

static void test_t3_inventory_cap() {
    std::cout << "\n=== T3. cap at 50, 60 input → 10 omitted ===\n\n";

    auto vault = make_temp_vault();
    for (int i = 0; i < 60; ++i) {
        auto idx = std::to_string(i);
        write_knowledge(vault, "rule-" + idx + ".md", "RULE_BODY_" + idx + "\n");
    }

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "agent-t3", vault, "turn", "task body T3");

    auto inv_pos = split.user_message.find("## Vault Inventory");
    check(inv_pos != std::string::npos,
          "T3: inventory header present");

    auto inv_body = split.user_message.substr(inv_pos);
    auto end_a = inv_body.find("---");
    auto end_b = inv_body.find("# Inbox:");
    auto end_c = inv_body.find("# Current Task");
    auto end = std::min({end_a, end_b, end_c});
    if (end != std::string::npos) inv_body = inv_body.substr(0, end);

    auto entries = count_inventory_entries(split.user_message);
    check(entries == 50,
          "T3: exactly 50 inventory entries (cap honored)");
    check(inv_body.find("[10 additional files omitted from inventory") != std::string::npos,
          "T3: transparency line lists '10 additional files omitted'");

    // Track 3: every retained line should end in a human-relative mtime.
    std::regex human_mtime(R"((just now|\d+m ago|\d+h ago|\d+d ago))");
    auto begin = std::sregex_iterator(inv_body.begin(), inv_body.end(),
                                       human_mtime);
    auto end_it = std::sregex_iterator();
    auto human_count = std::distance(begin, end_it);
    check(human_count >= 50,
          "T3: at least 50 human mtimes in inventory body (one per entry)");

    cleanup(vault);
}

// --- T4: empty — empty vault → no inventory section emitted ----------------

static void test_t4_empty_vault() {
    std::cout << "\n=== T4. empty vault → no '## Vault Inventory' section ===\n\n";

    auto vault = make_temp_vault();

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "agent-t4", vault, "turn", "task body T4");

    check(split.user_message.find("## Vault Inventory") == std::string::npos,
          "T4: no inventory section when vault is empty");
    // Sanity: prompt is otherwise non-empty (Current Task is always emitted).
    check(split.user_message.find("# Current Task") != std::string::npos,
          "T4: '# Current Task' still emitted (sanity)");

    cleanup(vault);
}

// --- T5: brainstorm scribe cross-vault inventory ---------------------------

static void test_t5_brainstorm_scribe_cross_vault() {
    std::cout << "\n=== T5. brainstorm scribe sees teammate vault knowledge ===\n\n";

    // Layout: <base>/vaults/{scribe-t5,doer-t5,leader-t5}/knowledge/
    auto base = fs::temp_directory_path() /
        ("quorum_test_inv_bs_" + std::to_string(getpid()) + "_" +
         std::to_string(g_test_num++));
    auto vaults_root = base / "vaults";
    auto scribe_vault = vaults_root / "scribe-t5";
    auto doer_vault = vaults_root / "doer-t5";
    auto leader_vault = vaults_root / "leader-t5";
    fs::create_directories(scribe_vault / "knowledge");
    fs::create_directories(doer_vault / "knowledge");
    fs::create_directories(leader_vault / "knowledge");

    write_file(scribe_vault / "knowledge" / "rule-scribe-self.md",
               "SCRIBE_OWN_RULE\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    write_file(doer_vault / "knowledge" / "rule-doer-side.md",
               "DOER_RULE\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    write_file(leader_vault / "knowledge" / "ref-leader-side.md",
               "LEADER_REF\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    sui::quorum::ContextAssembler assembler;

    // Brainstorm-mode scribe sees teammate knowledge.
    auto split_bs = assembler.assemble_split(
        "scribe-t5", scribe_vault.string(), "turn", "task body T5",
        /*team_roster=*/{}, /*skill_file=*/{}, /*project_root=*/{},
        /*agent_role=*/"scribe", /*budget=*/{},
        /*conversation_mode=*/"brainstorm");

    auto inv_pos = split_bs.user_message.find("## Vault Inventory");
    check(inv_pos != std::string::npos,
          "T5: brainstorm scribe inventory present");
    auto inv_body = split_bs.user_message.substr(inv_pos);
    auto end_a = inv_body.find("---");
    auto end_b = inv_body.find("# Inbox:");
    auto end_c = inv_body.find("# Current Task");
    auto end = std::min({end_a, end_b, end_c});
    if (end != std::string::npos) inv_body = inv_body.substr(0, end);

    check(inv_body.find("rule-scribe-self.md") != std::string::npos,
          "T5: scribe's own rule listed");
    // Track 3: cross-vault entries are metadata-only (no content read), so
    // tags stay empty → the `[tags]` block is omitted entirely. The
    // literal "<filename> (vault: <agent>)" pattern still holds.
    check(inv_body.find("rule-doer-side.md (vault: doer-t5)") != std::string::npos,
          "T5: doer-side rule listed with 'vault: doer-t5' label (no tag block)");
    check(inv_body.find("ref-leader-side.md (vault: leader-t5)") != std::string::npos,
          "T5: leader-side ref listed with 'vault: leader-t5' label (no tag block)");

    // Generic mode → teammate files NOT included.
    auto split_generic = assembler.assemble_split(
        "scribe-t5", scribe_vault.string(), "turn", "task body T5",
        /*team_roster=*/{}, /*skill_file=*/{}, /*project_root=*/{},
        /*agent_role=*/"scribe", /*budget=*/{},
        /*conversation_mode=*/"generic");

    auto inv_pos_g = split_generic.user_message.find("## Vault Inventory");
    check(inv_pos_g != std::string::npos,
          "T5: generic-mode scribe still has its own inventory");
    auto inv_body_g = split_generic.user_message.substr(inv_pos_g);
    auto end_ag = inv_body_g.find("---");
    auto end_bg = inv_body_g.find("# Inbox:");
    auto end_cg = inv_body_g.find("# Current Task");
    auto end_g = std::min({end_ag, end_bg, end_cg});
    if (end_g != std::string::npos) inv_body_g = inv_body_g.substr(0, end_g);

    check(inv_body_g.find("rule-doer-side.md") == std::string::npos,
          "T5: generic mode does NOT pull doer-side rule into inventory");
    check(inv_body_g.find("ref-leader-side.md") == std::string::npos,
          "T5: generic mode does NOT pull leader-side ref into inventory");

    cleanup(base.string());
}

// --- T6: tagged ref — frontmatter tags surface as `[a, b]` block -----------

static void test_t6_tagged_ref() {
    std::cout << "\n=== T6. tagged ref renders [tags] block ===\n\n";

    auto vault = make_temp_vault();
    // Ref with frontmatter tags. Track 2 caches RefEntry::tags from the
    // same parse helper Track 3 reuses for rules.
    write_knowledge(vault, "ref-tagged.md",
                    "---\n"
                    "title: Tagged Ref\n"
                    "tags: [walrus, seal]\n"
                    "---\n"
                    "REF_BODY_TAGGED\n");

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "agent-t6", vault, "turn", "task body T6");

    auto inv_pos = split.user_message.find("## Vault Inventory");
    check(inv_pos != std::string::npos, "T6: inventory header present");
    auto inv_body = split.user_message.substr(inv_pos);
    auto end_a = inv_body.find("---");
    auto end_b = inv_body.find("# Inbox:");
    auto end_c = inv_body.find("# Current Task");
    auto end = std::min({end_a, end_b, end_c});
    if (end != std::string::npos) inv_body = inv_body.substr(0, end);

    // The line should be: "- ref-tagged.md [walrus, seal] (vault: agent-t6) — <human mtime>"
    check(inv_body.find("ref-tagged.md [walrus, seal] (vault: agent-t6)") !=
          std::string::npos,
          "T6: ref-tagged.md renders with '[walrus, seal]' tag block before scope");

    cleanup(vault);
}

// --- T7: untagged rule — no `[tags]` block emitted -------------------------

static void test_t7_untagged_rule() {
    std::cout << "\n=== T7. untagged rule has no [tags] block ===\n\n";

    auto vault = make_temp_vault();
    // Frontmatter present but no tags key → empty tag list → no brackets.
    write_knowledge(vault, "rule-no-tags.md",
                    "---\n"
                    "title: Untagged Rule\n"
                    "---\n"
                    "RULE_BODY_NO_TAGS\n");
    // Bare body (no frontmatter at all) — also empty tag list.
    write_knowledge(vault, "rule-bare.md", "JUST_A_BODY\n");

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "agent-t7", vault, "turn", "task body T7");

    auto inv_pos = split.user_message.find("## Vault Inventory");
    check(inv_pos != std::string::npos, "T7: inventory header present");
    auto inv_body = split.user_message.substr(inv_pos);
    auto end_a = inv_body.find("---");
    auto end_b = inv_body.find("# Inbox:");
    auto end_c = inv_body.find("# Current Task");
    auto end = std::min({end_a, end_b, end_c});
    if (end != std::string::npos) inv_body = inv_body.substr(0, end);

    // Both should go straight from filename to "(vault: agent-t7)".
    check(inv_body.find("rule-no-tags.md (vault: agent-t7)") != std::string::npos,
          "T7: rule with frontmatter but no tags key has no [tags] block");
    check(inv_body.find("rule-bare.md (vault: agent-t7)") != std::string::npos,
          "T7: rule with no frontmatter at all has no [tags] block");

    // Defensive: there should be no empty bracket pair like "rule-... [] ("
    check(inv_body.find("[]") == std::string::npos,
          "T7: no phantom empty '[]' anywhere in inventory body");

    cleanup(vault);
}

// --- T8: mixed vault — tagged and untagged in the same inventory -----------

static void test_t8_mixed_vault() {
    std::cout << "\n=== T8. mixed vault (tagged + untagged interleaved) ===\n\n";

    auto vault = make_temp_vault();
    write_knowledge(vault, "rule-tagged-mix.md",
                    "---\n"
                    "tags: [alpha]\n"
                    "---\n"
                    "TAGGED_MIX_BODY\n");
    write_knowledge(vault, "rule-untagged-mix.md", "UNTAGGED_MIX_BODY\n");
    write_knowledge(vault, "ref-tagged-mix.md",
                    "---\n"
                    "tags: [beta, gamma]\n"
                    "---\n"
                    "REF_TAGGED_MIX_BODY\n");
    write_knowledge(vault, "ref-untagged-mix.md", "REF_UNTAGGED_MIX_BODY\n");

    sui::quorum::ContextAssembler assembler;
    auto split = assembler.assemble_split(
        "agent-t8", vault, "turn", "task body T8");

    auto inv_pos = split.user_message.find("## Vault Inventory");
    check(inv_pos != std::string::npos, "T8: inventory header present");
    auto inv_body = split.user_message.substr(inv_pos);
    auto end_a = inv_body.find("---");
    auto end_b = inv_body.find("# Inbox:");
    auto end_c = inv_body.find("# Current Task");
    auto end = std::min({end_a, end_b, end_c});
    if (end != std::string::npos) inv_body = inv_body.substr(0, end);

    check(inv_body.find("rule-tagged-mix.md [alpha] (vault: agent-t8)") !=
          std::string::npos,
          "T8: rule-tagged-mix.md renders with [alpha] block");
    check(inv_body.find("rule-untagged-mix.md (vault: agent-t8)") !=
          std::string::npos,
          "T8: rule-untagged-mix.md renders without tag block");
    check(inv_body.find("ref-tagged-mix.md [beta, gamma] (vault: agent-t8)") !=
          std::string::npos,
          "T8: ref-tagged-mix.md renders with [beta, gamma] block");
    check(inv_body.find("ref-untagged-mix.md (vault: agent-t8)") !=
          std::string::npos,
          "T8: ref-untagged-mix.md renders without tag block");

    // Sanity: every line ends in a human-relative mtime, regardless of tags.
    std::regex human_mtime(R"((just now|\d+m ago|\d+h ago|\d+d ago))");
    auto begin = std::sregex_iterator(inv_body.begin(), inv_body.end(),
                                       human_mtime);
    auto end_it = std::sregex_iterator();
    auto human_count = std::distance(begin, end_it);
    check(human_count >= 4,
          "T8: at least 4 human mtimes in inventory body");

    cleanup(vault);
}

// --- main -------------------------------------------------------------------

int main() {
    std::cout << "=====================================================\n";
    std::cout << "  Phase 9 Track 1 + Track 3 — vault inventory tests\n";
    std::cout << "=====================================================\n";

    test_t1_basic_inventory();
    test_t2_scope_coverage();
    test_t3_inventory_cap();
    test_t4_empty_vault();
    test_t5_brainstorm_scribe_cross_vault();
    test_t6_tagged_ref();
    test_t7_untagged_rule();
    test_t8_mixed_vault();

    std::cout << "\n---------------------------------------------------\n";
    std::cout << "  passed: " << g_passed << "  failed: " << g_failed << "\n";
    std::cout << "---------------------------------------------------\n";

    return g_failed == 0 ? 0 : 1;
}
