#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "utils/config.h"

namespace sui::quorum {

struct ContextBudget {
    size_t max_files = 20;
    size_t max_chars = 200000;  // rough proxy for tokens (~4 chars/token)
};

// Hard cap on rule files preloaded into the prompt. Total across all scopes —
// project (.quorum/knowledge/), role (.quorum/knowledge/roles/<role>/), and
// agent vault (<vault>/knowledge/) share this single budget. When the rule
// union exceeds MAX_RULES, oldest are evicted and a transparency note is
// appended to the prompt.
constexpr size_t MAX_RULES = 10;

// Filename-based classification of knowledge files. Used by assemble() to
// decide whether a file is preloaded (rule), search-only (reference, Track 4),
// or recency-budget-loaded (plain).
enum class KnowledgeKind {
    Rule,        // filename starts with "rule-"
    Reference,   // filename starts with "ref-"
    Plain,       // anything else
};

// Pure helper — classify a knowledge filename. Exposed for testability and
// reused by Tracks 2-4. Does NOT touch the filesystem.
[[nodiscard]] inline KnowledgeKind classify_knowledge_filename(const std::string& filename) {
    if (filename.rfind("rule-", 0) == 0) return KnowledgeKind::Rule;
    if (filename.rfind("ref-", 0) == 0) return KnowledgeKind::Reference;
    return KnowledgeKind::Plain;
}

// Assembles context for agent invocation from vault contents + task description.
// Reads CONTEXT.md (always), then recent knowledge files up to budget.
class ContextAssembler {
public:
    // Build a team roster + routing instructions for team mode prompts.
    [[nodiscard]] static std::string build_roster(
        const std::vector<AgentMetadata>& agents,
        const std::string& current_agent_id,
        const ConversationConfig& conv_cfg) {

        std::string roster;
        roster += "## Your Team\n\n";
        for (const auto& a : agents) {
            roster += "- **" + a.id + "**";
            if (!a.role.empty()) roster += " (" + a.role + ")";
            if (a.id == current_agent_id) roster += " <- you";
            if (!a.description.empty()) roster += " -- " + a.description;
            roster += "\n";
        }

        roster += "\n## Routing\n\n";
        if (!conv_cfg.default_path.empty()) {
            roster += "A default path is configured: ";
            for (size_t i = 0; i < conv_cfg.default_path.size(); ++i) {
                if (i > 0) roster += " -> ";
                roster += conv_cfg.default_path[i];
            }
            roster += " -> done\n";
            roster += "You do not need to include a HANDOFF block unless you want "
                      "to override the default routing.\n\n";
        } else {
            roster += "No default path is configured. You must include a HANDOFF "
                      "block to specify who should go next.\n\n";
        }

        roster += "To override routing or specify the next agent, output a HANDOFF block:\n\n";
        roster += "```HANDOFF\n";
        roster += "to: <agent_id | human | done>\n";
        roster += "prompt: <instructions for the next agent>\n";
        roster += "```\n\n";

        if (!conv_cfg.leader.empty()) {
            roster += "If you are unsure who should go next, do not include a HANDOFF block -- "
                      "the ball will return to **" + conv_cfg.leader + "**.\n\n";
        }

        return roster;
    }

    // Build a prompt string from agent vault + task description.
    //
    // `agent_role` selects the role-scope subdirectory used for role-scoped
    // rules: <project_root>/.quorum/knowledge/roles/<agent_role>/. When empty
    // (or when project_root is empty), role-scope resolution is skipped.
    [[nodiscard]] std::string assemble(const std::string& agent_name,
                                        const std::string& vault_dir,
                                        const std::string& task_type,
                                        const std::string& task_description,
                                        const std::string& team_roster = {},
                                        const std::string& skill_file = {},
                                        const std::string& project_root = {},
                                        const std::string& agent_role = {},
                                        ContextBudget budget = {}) const {
        std::string prompt;
        size_t files_loaded = 0;

        // Always load CONTEXT.md first
        auto context_path = std::filesystem::path(vault_dir) / "CONTEXT.md";
        if (std::filesystem::exists(context_path)) {
            auto content = read_file(context_path);
            if (!content.empty()) {
                prompt += "# Agent Context\n\n";
                prompt += content;
                prompt += "\n\n";
                ++files_loaded;
            }
        }

        // Load SKILL.md if provided
        if (!skill_file.empty()) {
            std::string spath = skill_file;
            // 1. Expand ~/
            if (spath.starts_with("~/")) {
                auto home = std::getenv("HOME");
                if (home) spath = std::string(home) + spath.substr(1);
            }
            auto skill_path = std::filesystem::path(spath);
            // 2. If relative and project_root provided, try resolving from project root
            if (skill_path.is_relative() && !project_root.empty()) {
                auto rooted = std::filesystem::path(project_root) / skill_path;
                if (std::filesystem::exists(rooted)) {
                    skill_path = rooted;
                }
            }
            // 3. Load
            if (std::filesystem::exists(skill_path)) {
                auto content = read_file(skill_path);
                if (!content.empty()) {
                    prompt += "# Skill Reference\n\n";
                    prompt += content;
                    prompt += "\n\n";
                    ++files_loaded;
                }
            }
        }

        // Load knowledge files. Phase 7 Track 1+2+3: filename-aware loading,
        // resolved across THREE scopes (project → role → agent vault):
        //   - project: <project_root>/.quorum/knowledge/                    (all agents)
        //   - role:    <project_root>/.quorum/knowledge/roles/<agent_role>/ (every agent of this role)
        //   - vault:   <vault_dir>/knowledge/                               (this agent only)
        //
        //   rule-*.md  → preloaded, sorted by recency, hard-capped at MAX_RULES
        //                (cap operates on the UNION across all 3 scopes)
        //   ref-*.md   → NOT preloaded (search-on-demand in Track 4)
        //   anything else → plain narrative; recency-loaded under remaining budget
        //
        // Dedup priority (most-specific wins): agent > role > project. If the
        // SAME content (std::hash match) appears in multiple scopes, only the
        // most-specific copy is emitted; the rest are suppressed silently —
        // NOT counted as cap evictions. The agent's intent is identical
        // regardless of which file the daemon ultimately loads.
        //
        // Scope annotations in the emitted '# Knowledge:' header:
        //   (project)         | (role: <role>)         | (vault: <agent>)
        //
        // Note: filesystem mtime stands in for createdAt. Once the daemon
        // tracks createdAt explicitly (Track 5/cache work), swap to that.

        // A loaded knowledge entry tagged with its origin scope. `scope_rank`
        // encodes specificity (lower = more specific) for dedup tie-breaking:
        //   0 = agent vault, 1 = role, 2 = project.
        struct ScopedFile {
            std::filesystem::path path;
            std::filesystem::file_time_type mtime;
            std::string scope_label;  // "project" | "role: <role>" | "vault: <agent>"
            std::string content;      // read once, reused for hash + emit
            int scope_rank = 2;       // 0 agent, 1 role, 2 project
        };

        auto scope_for_vault = std::string("vault: ") + agent_name;
        auto scope_for_role = agent_role.empty()
            ? std::string{}
            : std::string("role: ") + agent_role;

        // Walk one scope and partition by filename kind. Reads file content
        // eagerly so dedup can hash without re-reading.
        auto walk_scope = [&](const std::filesystem::path& dir,
                              const std::string& scope_label,
                              int scope_rank,
                              std::vector<ScopedFile>& rules_out,
                              std::vector<ScopedFile>& plains_out) {
            if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) return;
            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                if (!entry.is_regular_file()) continue;
                auto kind = classify_knowledge_filename(entry.path().filename().string());
                if (kind == KnowledgeKind::Reference) continue;  // Track 4

                ScopedFile sf;
                sf.path = entry.path();
                sf.mtime = entry.last_write_time();
                sf.scope_label = scope_label;
                sf.scope_rank = scope_rank;
                sf.content = read_file(sf.path);
                if (sf.content.empty()) continue;

                if (kind == KnowledgeKind::Rule) {
                    rules_out.push_back(std::move(sf));
                } else {
                    plains_out.push_back(std::move(sf));
                }
            }
        };

        std::vector<ScopedFile> all_rules;
        std::vector<ScopedFile> all_plains;

        // Project scope (resolves to <project_root>/.quorum/knowledge/).
        // Skipped silently when project_root unset (e.g. unit tests in /tmp).
        if (!project_root.empty()) {
            auto project_knowledge = std::filesystem::path(project_root) / ".quorum" / "knowledge";
            walk_scope(project_knowledge, "project", /*scope_rank=*/2, all_rules, all_plains);

            // Role scope (resolves to <project_root>/.quorum/knowledge/roles/<agent_role>/).
            // Skipped silently when agent_role is empty.
            if (!agent_role.empty()) {
                auto role_knowledge = std::filesystem::path(project_root) /
                    ".quorum" / "knowledge" / "roles" / agent_role;
                walk_scope(role_knowledge, scope_for_role, /*scope_rank=*/1,
                           all_rules, all_plains);
            }
        }

        // Agent vault scope.
        auto vault_knowledge = std::filesystem::path(vault_dir) / "knowledge";
        walk_scope(vault_knowledge, scope_for_vault, /*scope_rank=*/0,
                   all_rules, all_plains);

        // Dedup priority (agent > role > project): for any group of entries
        // with identical content, keep only the most-specific (lowest
        // scope_rank). Suppression is silent and does NOT consume the
        // eviction budget — duplicate content is the same rule, just
        // colocated across scopes.
        auto dedup_by_specificity = [](std::vector<ScopedFile>& entries) {
            if (entries.size() < 2) return;
            // Per-content best rank seen so far.
            std::unordered_map<size_t, int> best_rank;
            best_rank.reserve(entries.size());
            for (const auto& sf : entries) {
                auto h = std::hash<std::string>{}(sf.content);
                auto it = best_rank.find(h);
                if (it == best_rank.end() || sf.scope_rank < it->second) {
                    best_rank[h] = sf.scope_rank;
                }
            }
            std::vector<ScopedFile> kept;
            kept.reserve(entries.size());
            for (auto& sf : entries) {
                auto h = std::hash<std::string>{}(sf.content);
                if (sf.scope_rank == best_rank[h]) {
                    kept.push_back(std::move(sf));
                }
                // else: a more-specific scope has the same content → drop.
            }
            entries = std::move(kept);
        };
        dedup_by_specificity(all_rules);
        dedup_by_specificity(all_plains);

        // Sort by mtime DESC so most-recent wins regardless of scope.
        auto by_recency_desc = [](const ScopedFile& a, const ScopedFile& b) {
            return a.mtime > b.mtime;
        };
        std::sort(all_rules.begin(), all_rules.end(), by_recency_desc);
        std::sort(all_plains.begin(), all_plains.end(), by_recency_desc);

        // Apply MAX_RULES cap on the UNION of both scopes (recency wins).
        size_t evicted = 0;
        if (all_rules.size() > MAX_RULES) {
            evicted = all_rules.size() - MAX_RULES;
            all_rules.resize(MAX_RULES);
        }

        // Emit rules with scope annotation. Header format:
        //   # Knowledge: <filename> (<scope>)
        // where scope is "project" or "vault: <agent>".
        bool emitted_any_knowledge = false;
        for (const auto& sf : all_rules) {
            if (files_loaded >= budget.max_files) break;
            if (prompt.size() >= budget.max_chars) break;

            prompt += "# Knowledge: " + sf.path.filename().string() +
                      " (" + sf.scope_label + ")\n\n";
            prompt += sf.content;
            prompt += "\n\n";
            ++files_loaded;
            emitted_any_knowledge = true;
        }

        // Transparency note for cap eviction (NOT for dedup suppression).
        if (evicted > 0) {
            prompt += "[" + std::to_string(evicted) +
                     " rules omitted — most-recent " + std::to_string(MAX_RULES) +
                     " preserved; rename older rules or rotate as needed]\n\n";
        }

        // Plain narratives — bucket-ordered after rules, share remaining budget.
        for (const auto& sf : all_plains) {
            if (files_loaded >= budget.max_files) break;
            if (prompt.size() >= budget.max_chars) break;

            prompt += "# Knowledge: " + sf.path.filename().string() +
                      " (" + sf.scope_label + ")\n\n";
            prompt += sf.content;
            prompt += "\n\n";
            ++files_loaded;
            emitted_any_knowledge = true;
        }
        (void)emitted_any_knowledge;  // reserved for future tracing

        // Load inbox items
        auto inbox_dir = std::filesystem::path(vault_dir) / "inbox";
        if (std::filesystem::exists(inbox_dir) && std::filesystem::is_directory(inbox_dir)) {
            auto files = list_files_by_recency(inbox_dir);
            for (const auto& f : files) {
                if (files_loaded >= budget.max_files) break;
                if (prompt.size() >= budget.max_chars) break;

                auto content = read_file(f);
                if (!content.empty()) {
                    prompt += "# Inbox: " + f.filename().string() + "\n\n";
                    prompt += content;
                    prompt += "\n\n";
                    ++files_loaded;
                }
            }
        }

        // Inject team roster if provided (team mode)
        if (!team_roster.empty()) {
            prompt += "---\n\n";
            prompt += team_roster;
        }

        // Append task
        prompt += "---\n\n";
        prompt += "# Current Task\n\n";
        prompt += "**Task type:** " + task_type + "\n";
        prompt += "**Agent:** " + agent_name + "\n\n";
        prompt += task_description;
        prompt += "\n";

        // Legacy output rules — only when NOT in team mode
        if (team_roster.empty()) {
            prompt += "\n";
            prompt += "---\n\n";
            prompt += "# CRITICAL — Output Rules\n\n";
            prompt += "You MUST follow these rules for ALL output:\n\n";
            prompt += "1. **NEVER write files directly.** Do not use Write, Edit, or any file-creation tool. ";
            prompt += "All output goes in your response text as structured blocks.\n";
            prompt += "2. **NEVER run commands that modify files.** You may READ files and RUN queries ";
            prompt += "(sqlite3, cat, ls, grep), but never write, move, or delete.\n";
            prompt += "3. **ALL findings must use structured blocks** in your response: ";
            prompt += "VAULT_UPDATE, OBSERVATION, PROPOSAL, SUMMARY.\n";
            prompt += "4. **Only write to YOUR vault.** VAULT_UPDATE paths must start with `knowledge/` or `inbox/`.\n\n";
            prompt += "The daemon extracts these blocks from your response text and routes them. ";
            prompt += "If you write files directly, the daemon cannot track your output.\n\n";

            // Append output format instructions
            prompt += "---\n\n";
            prompt += "# Output Instructions\n\n";
            prompt += "When you have findings, use these structured blocks in your response:\n\n";
            prompt += "- **VAULT_UPDATE**: Your current distilled beliefs. Overwrites previous. Keep concise.\n";
            prompt += "- **OBSERVATION**: What you noticed. Timestamped, accumulated over time. Write freely.\n";
            prompt += "- **PROPOSAL**: Actions requiring consensus from other agents.\n\n";
            prompt += "```VAULT_UPDATE\n";
            prompt += "path: knowledge/<filename>.md\n";
            prompt += "content: |\n";
            prompt += "  <content to write>\n";
            prompt += "```\n\n";
            prompt += "```PROPOSAL\n";
            prompt += "title: <title>\n";
            prompt += "requires_consensus_from: [<agent_names>]\n";
            prompt += "content: |\n";
            prompt += "  <proposal details>\n";
            prompt += "```\n\n";
            prompt += "```OBSERVATION\n";
            prompt += "title: <what you observed>\n";
            prompt += "tags: [<relevant, topic, tags>]\n";
            prompt += "content: |\n";
            prompt += "  <detailed observation -- accumulated, never overwritten>\n";
            prompt += "```\n\n";
            prompt += "```SUMMARY\n";
            prompt += "<brief findings summary>\n";
            prompt += "```\n";
        }

        return prompt;
    }

private:
    [[nodiscard]] static std::string read_file(const std::filesystem::path& path) {
        std::ifstream f(path);
        if (!f.is_open()) return {};
        return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    }

    // List files sorted by modification time (most recent first)
    [[nodiscard]] static std::vector<std::filesystem::path> list_files_by_recency(const std::filesystem::path& dir) {
        std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> entries;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.is_regular_file()) {
                entries.push_back({entry.last_write_time(), entry.path()});
            }
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });

        std::vector<std::filesystem::path> result;
        result.reserve(entries.size());
        for (auto& [_, path] : entries) {
            result.push_back(std::move(path));
        }
        return result;
    }
};

} // namespace sui::quorum
