#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "utils/config.h"
#include "utils/frontmatter.h"

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

// ---------------------------------------------------------------------------
// Phase 7 Track 4 — search_knowledge ref retrieval
// ---------------------------------------------------------------------------
//
// Refs (ref-*.md) are partitioned out of the rule preload (Track 1-3) and
// surface only when relevant to the agent's current task. The daemon scores
// each ref against the task prompt as the implicit query and emits the top-K
// matches in a "## Searched References" section of the assembled prompt.
//
// Scoring is deliberately simple: tokenize the query, count matches in the
// filename and content, weight filename matches 3x higher (filenames are
// deliberate signal). No embeddings, no cosine, no MCP server. Agents can
// read full content via the existing Read tool if an excerpt looks relevant.
// If real usage ever demands explicit invocation, Phase 8 can add MCP.

// One scored ref candidate produced by search_references(). Exposed here for
// testability and so the assembler can render it directly into the prompt.
struct ScoredRef {
    std::filesystem::path path;
    int scope_rank = 2;       // 0 agent, 1 role, 2 project (lower = more specific)
    std::string scope_label;  // "vault: <agent>" | "role: <role>" | "project"
    std::string excerpt;      // ~200 chars, frontmatter + leading H1 stripped
    int score = 0;            // higher = better match; 0 → not surfaced
    std::filesystem::file_time_type mtime{};  // tie-break (DESC)
};

// A ref entry produced by walking the 3-scope hierarchy. The assembler builds
// these once per assemble() and feeds them to search_references().
struct RefEntry {
    std::filesystem::path path;
    std::string scope_label;
    int scope_rank = 2;
    std::string content;
    std::filesystem::file_time_type mtime{};
    std::vector<std::string> tags;  // Phase 9 Track 2: cached frontmatter tags
};

namespace detail {

// Tiny English stopword list. Kept short and inline — these are the tokens
// most likely to drown out real signal in short task prompts. Larger lists
// risk dropping legitimate domain terms (e.g. "do" in "DOS").
[[nodiscard]] inline bool is_stopword(const std::string& tok) {
    static const std::unordered_set<std::string> sw = {
        "a", "an", "the", "is", "of", "to", "in", "for", "with", "on", "at"
    };
    return sw.find(tok) != sw.end();
}

// Lowercase + split on whitespace and ASCII punctuation. Drops stopwords
// and very short (<2 chars) tokens. No regex — std::isalnum check only.
[[nodiscard]] inline std::vector<std::string> tokenize_lower(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    cur.reserve(16);
    auto flush = [&]() {
        if (cur.size() >= 2 && !is_stopword(cur)) out.push_back(cur);
        cur.clear();
    };
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            cur.push_back(static_cast<char>(std::tolower(uc)));
        } else {
            flush();
        }
    }
    flush();
    return out;
}

// Count occurrences of any query token in `haystack` (lowercase substring
// match). Each token contributes its match count — multi-occurrence content
// scores higher, which is the desired heuristic.
[[nodiscard]] inline int count_token_matches(
    const std::string& haystack_lower,
    const std::vector<std::string>& tokens) {
    int total = 0;
    for (const auto& t : tokens) {
        if (t.empty()) continue;
        size_t pos = 0;
        while ((pos = haystack_lower.find(t, pos)) != std::string::npos) {
            ++total;
            pos += t.size();
        }
    }
    return total;
}

// Lowercase a string in-place copy.
[[nodiscard]] inline std::string to_lower_copy(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(c))));
    }
    return out;
}

// Extract a prompt-friendly ~200-char excerpt:
//   - skip a leading YAML frontmatter block ('---' line ... '---' line)
//   - skip a leading H1 line ('# Title')
//   - collapse runs of whitespace (incl. newlines) into single spaces
[[nodiscard]] inline std::string make_excerpt(const std::string& content,
                                              size_t max_chars = 200) {
    size_t i = 0;
    const size_t n = content.size();

    // Skip leading whitespace.
    while (i < n && (content[i] == '\n' || content[i] == '\r' ||
                     content[i] == ' ' || content[i] == '\t')) {
        ++i;
    }

    // Skip YAML frontmatter if present: '---' on its own line ... '---'.
    if (i + 3 <= n && content.compare(i, 3, "---") == 0 &&
        (i + 3 == n || content[i + 3] == '\n' || content[i + 3] == '\r')) {
        // Advance past the opening '---' line.
        size_t lf = content.find('\n', i);
        if (lf == std::string::npos) {
            i = n;  // unterminated; nothing to extract
        } else {
            size_t scan = lf + 1;
            // Find the closing '---' line.
            while (scan < n) {
                size_t end = content.find('\n', scan);
                std::string line = (end == std::string::npos)
                    ? content.substr(scan)
                    : content.substr(scan, end - scan);
                // Trim trailing CR.
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line == "---") {
                    i = (end == std::string::npos) ? n : end + 1;
                    break;
                }
                if (end == std::string::npos) {
                    // Unterminated frontmatter — give up and treat as content.
                    break;
                }
                scan = end + 1;
            }
        }
    }

    // Skip leading whitespace again.
    while (i < n && (content[i] == '\n' || content[i] == '\r' ||
                     content[i] == ' ' || content[i] == '\t')) {
        ++i;
    }

    // Skip a single leading H1 line ('# Title').
    if (i < n && content[i] == '#') {
        // Only strip if it's '#' followed by space (markdown H1/H2/etc).
        size_t h = i;
        while (h < n && content[h] == '#') ++h;
        if (h < n && content[h] == ' ') {
            size_t lf = content.find('\n', i);
            i = (lf == std::string::npos) ? n : lf + 1;
        }
    }

    // Skip leading whitespace once more before collecting.
    while (i < n && (content[i] == '\n' || content[i] == '\r' ||
                     content[i] == ' ' || content[i] == '\t')) {
        ++i;
    }

    // Collect chars, collapsing whitespace runs into single spaces.
    std::string out;
    out.reserve(max_chars);
    bool in_ws = false;
    for (; i < n && out.size() < max_chars; ++i) {
        char c = content[i];
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!in_ws && !out.empty()) {
                out.push_back(' ');
                in_ws = true;
            }
        } else {
            out.push_back(c);
            in_ws = false;
        }
    }
    // Trim trailing space.
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace detail

// Score `refs` against `query` and return the top `top_k` matches with
// non-zero score, ordered by score DESC, mtime DESC tie-break. Pure
// function — no filesystem access (refs are pre-loaded by the caller).
[[nodiscard]] inline std::vector<ScoredRef> search_references(
    const std::vector<RefEntry>& refs,
    const std::string& query,
    size_t top_k = 5) {

    auto tokens = detail::tokenize_lower(query);
    if (tokens.empty() || refs.empty()) return {};

    std::vector<ScoredRef> scored;
    scored.reserve(refs.size());

    for (const auto& r : refs) {
        auto fname_lower = detail::to_lower_copy(r.path.filename().string());
        auto content_lower = detail::to_lower_copy(r.content);

        int fn_hits = detail::count_token_matches(fname_lower, tokens);
        int content_hits = detail::count_token_matches(content_lower, tokens);

        // Phase 9 Track 2 — exact (case-insensitive) whole-tag matches
        // between query tokens and the ref's cached frontmatter tags.
        // Each (query_token, tag) equality contributes 1; weight ×5.
        // Tags were already lowercased at parse time.
        int tag_hits = 0;
        for (const auto& tok : tokens) {
            for (const auto& tag : r.tags) {
                if (tok == tag) ++tag_hits;
            }
        }

        int score = fn_hits * 3 + tag_hits * 5 + content_hits;
        if (score <= 0) continue;

        ScoredRef s;
        s.path = r.path;
        s.scope_rank = r.scope_rank;
        s.scope_label = r.scope_label;
        s.excerpt = detail::make_excerpt(r.content);
        s.score = score;
        s.mtime = r.mtime;
        scored.push_back(std::move(s));
    }

    std::sort(scored.begin(), scored.end(),
              [](const ScoredRef& a, const ScoredRef& b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.mtime > b.mtime;  // recency tie-break
              });

    if (scored.size() > top_k) scored.resize(top_k);
    return scored;
}

// Phase 7 Track 5 — system-prompt split for Anthropic prefix-cache reuse.
//
// `claude -p --append-system-prompt-file <path>` appends to Claude Code's
// default system prompt. Stable identity (CONTEXT.md + SKILL.md + output
// rules) goes in system_prompt; per-task variable content (rules, refs,
// inbox, roster, current task) stays in user_message and is piped via stdin.
// Two consecutive turns for the same agent now share an identical prefix,
// which lets the API charge `cache_read_input_tokens` instead of
// `cache_creation_input_tokens` on the second hit.
struct AssembledPrompt {
    std::string system_prompt;   // stable per agent — appended via --append-system-prompt-file
    std::string user_message;    // varies per task — piped via stdin
};

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
        // Phase 8 Track 7 (#31): the daemon's `default_path` is internal
        // routing logic and MUST NOT be advertised to agents. When it was
        // surfaced (Phase 7), agents misread path entries as legitimate
        // HANDOFF targets — e.g. an architect routed to "thinker" because
        // "thinker" appeared in the printed path. Routing is enforced
        // implicitly by the daemon; the agent only needs to know the
        // override mechanism (HANDOFF block) and the no-handoff fallback.
        roster += "If you want to specify the next agent, output a HANDOFF block. "
                  "Use the YAML block-scalar form (`prompt: |` then 2-space-indented "
                  "lines) for multi-line instructions — a bare `prompt: text` reads "
                  "only up to the end of the first line; everything after is dropped.\n\n";
        roster += "```HANDOFF\n";
        roster += "to: <agent_id | human | done>\n";
        roster += "prompt: |\n";
        roster += "  <instructions for the next agent>\n";
        roster += "  (indent every continuation line by exactly 2 spaces)\n";
        roster += "```\n\n";

        if (!conv_cfg.leader.empty()) {
            roster += "If you are unsure who should go next, do not include a HANDOFF block -- "
                      "the ball will return to **" + conv_cfg.leader + "**.\n\n";
        }

        return roster;
    }

    // Legacy single-string entry point. Phase 7 Track 5 made assemble_split()
    // the canonical builder; assemble() is now a thin shim that concatenates
    // system_prompt + user_message with the historical "\n---\n\n" glue so
    // pre-Track-5 callers and tests (test_assembler_rule_cap) see the same
    // byte layout.
    [[nodiscard]] std::string assemble(const std::string& agent_name,
                                        const std::string& vault_dir,
                                        const std::string& task_type,
                                        const std::string& task_description,
                                        const std::string& team_roster = {},
                                        const std::string& skill_file = {},
                                        const std::string& project_root = {},
                                        const std::string& agent_role = {},
                                        ContextBudget budget = {},
                                        const std::string& conversation_mode = {}) const {
        auto split = assemble_split(agent_name, vault_dir, task_type,
                                    task_description, team_roster, skill_file,
                                    project_root, agent_role, budget,
                                    conversation_mode);
        if (split.system_prompt.empty()) return split.user_message;
        return split.system_prompt + "\n---\n\n" + split.user_message;
    }

    // Phase 7 Track 5 — split the assembled prompt into a stable system_prompt
    // (CONTEXT.md + SKILL.md + output rules) and a per-task user_message
    // (rules, refs, inbox, roster, current task).
    //
    // `agent_role` selects the role-scope subdirectory used for role-scoped
    // rules: <project_root>/.quorum/knowledge/roles/<agent_role>/. When empty
    // (or when project_root is empty), role-scope resolution is skipped.
    [[nodiscard]] AssembledPrompt assemble_split(
        const std::string& agent_name,
        const std::string& vault_dir,
        const std::string& task_type,
        const std::string& task_description,
        const std::string& team_roster = {},
        const std::string& skill_file = {},
        const std::string& project_root = {},
        const std::string& agent_role = {},
        ContextBudget budget = {},
        const std::string& conversation_mode = {}) const {
        AssembledPrompt out;
        std::string& system_prompt = out.system_prompt;
        std::string& prompt = out.user_message;
        size_t files_loaded = 0;

        // Always load CONTEXT.md first — stable identity, system_prompt.
        auto context_path = std::filesystem::path(vault_dir) / "CONTEXT.md";
        if (std::filesystem::exists(context_path)) {
            auto content = read_file(context_path);
            if (!content.empty()) {
                system_prompt += "# Agent Context\n\n";
                system_prompt += content;
                system_prompt += "\n\n";
                ++files_loaded;
            }
        }

        // Load SKILL.md if provided — stable identity, system_prompt.
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
                    system_prompt += "# Skill Reference\n\n";
                    system_prompt += content;
                    system_prompt += "\n\n";
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
            // Phase 9 Track 3: cached frontmatter tags for inventory rendering.
            // Populated only for rules (kind == Rule); plains skip the parse.
            std::vector<std::string> tags;
        };

        auto scope_for_vault = std::string("vault: ") + agent_name;
        auto scope_for_role = agent_role.empty()
            ? std::string{}
            : std::string("role: ") + agent_role;

        // Walk one scope and partition by filename kind. Reads file content
        // eagerly so dedup can hash without re-reading. Refs are collected
        // separately for Track 4 search-on-demand.
        auto walk_scope = [&](const std::filesystem::path& dir,
                              const std::string& scope_label,
                              int scope_rank,
                              std::vector<ScopedFile>& rules_out,
                              std::vector<ScopedFile>& plains_out,
                              std::vector<RefEntry>& refs_out) {
            if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) return;
            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                if (!entry.is_regular_file()) continue;
                auto kind = classify_knowledge_filename(entry.path().filename().string());

                if (kind == KnowledgeKind::Reference) {
                    // Track 4: collect refs for search-on-demand. Empty
                    // content is OK here — filename can still match.
                    RefEntry r;
                    r.path = entry.path();
                    r.scope_label = scope_label;
                    r.scope_rank = scope_rank;
                    r.mtime = entry.last_write_time();
                    auto raw = read_file(r.path);
                    // Phase 9 Track 2 — parse frontmatter tags once at walk
                    // time so search_references doesn't re-parse per query.
                    r.tags = parse_frontmatter_tags(raw);
                    // Phase 9 finding #27d — store body without frontmatter so
                    // tag words don't score as both tag-hits (×5) and content-hits.
                    r.content = strip_frontmatter(raw);
                    refs_out.push_back(std::move(r));
                    continue;
                }

                ScopedFile sf;
                sf.path = entry.path();
                sf.mtime = entry.last_write_time();
                sf.scope_label = scope_label;
                sf.scope_rank = scope_rank;
                sf.content = read_file(sf.path);
                if (sf.content.empty()) continue;

                if (kind == KnowledgeKind::Rule) {
                    // Phase 9 Track 3 — parse frontmatter tags once at walk
                    // time (parity with RefEntry from Track 2). Zero extra
                    // I/O: content was already read above.
                    sf.tags = parse_frontmatter_tags(sf.content);
                    rules_out.push_back(std::move(sf));
                } else {
                    plains_out.push_back(std::move(sf));
                }
            }
        };

        std::vector<ScopedFile> all_rules;
        std::vector<ScopedFile> all_plains;
        std::vector<RefEntry> all_refs;

        // Project scope (resolves to <project_root>/.quorum/knowledge/).
        // Skipped silently when project_root unset (e.g. unit tests in /tmp).
        if (!project_root.empty()) {
            auto project_knowledge = std::filesystem::path(project_root) / ".quorum" / "knowledge";
            walk_scope(project_knowledge, "project", /*scope_rank=*/2,
                       all_rules, all_plains, all_refs);

            // Role scope (resolves to <project_root>/.quorum/knowledge/roles/<agent_role>/).
            // Skipped silently when agent_role is empty.
            if (!agent_role.empty()) {
                auto role_knowledge = std::filesystem::path(project_root) /
                    ".quorum" / "knowledge" / "roles" / agent_role;
                walk_scope(role_knowledge, scope_for_role, /*scope_rank=*/1,
                           all_rules, all_plains, all_refs);
            }
        }

        // Agent vault scope.
        auto vault_knowledge = std::filesystem::path(vault_dir) / "knowledge";
        walk_scope(vault_knowledge, scope_for_vault, /*scope_rank=*/0,
                   all_rules, all_plains, all_refs);

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

        // Phase 9 Track 1 — snapshot the un-evicted rule list for the
        // ## Vault Inventory section. The inventory must surface ALL rules
        // the agent could plausibly target with a VAULT_UPDATE, not just
        // the recency-capped subset that gets preloaded.
        std::vector<ScopedFile> rules_for_inventory = all_rules;

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

        // Phase 7 Track 4: search refs against the task prompt and surface
        // the top-5 most relevant in a "## Searched References" section.
        // The query is the agent's task description (the implicit input to
        // each turn). Section is omitted when no ref scores > 0 to keep the
        // prompt clean.
        if (!all_refs.empty()) {
            constexpr size_t kRefTopK = 5;
            auto matches = search_references(all_refs, task_description, kRefTopK);
            if (!matches.empty()) {
                prompt += "## Searched References (top " +
                          std::to_string(matches.size()) + " of " +
                          std::to_string(all_refs.size()) +
                          " matched against your task)\n\n";
                for (const auto& m : matches) {
                    prompt += "### " + m.path.filename().string() +
                              " (" + m.scope_label + ") — score: " +
                              std::to_string(m.score) + "\n";
                    if (!m.excerpt.empty()) {
                        prompt += m.excerpt + "\n";
                    }
                    prompt += "\n";
                }
                prompt += "Use the Read tool to load full content if any look relevant.\n\n";
            }
        }

        // Phase 9 Track 1 — ## Vault Inventory
        //
        // Lists every rule-*.md and ref-*.md the agent's resolution scope can
        // see (project + role + agent vault, plus teammate vaults for the
        // brainstorm-mode scribe). Keeps the agent from defaulting to "create
        // new file" when an existing one would be the right VAULT_UPDATE
        // target.
        //
        // Phase 9 Track 3 — line format:
        //   `- <name> [t1, t2] (<scope>) — <human mtime>`
        // Bracketed tag list is omitted entirely when the file's frontmatter
        // has no tags (or for cross-vault metadata-only entries where we
        // intentionally don't parse content).
        //
        // Sourced from `rules_for_inventory` (the pre-eviction copy) and
        // `all_refs` so cap-evicted rules still appear in the inventory.
        // The 50-entry cap operates on the union sorted by mtime DESC.
        {
            struct InventoryEntry {
                std::string filename;
                std::string scope_label;
                std::filesystem::file_time_type mtime;
                std::vector<std::string> tags;  // Track 3: rendered as [a, b]
            };
            std::vector<InventoryEntry> inventory;
            inventory.reserve(rules_for_inventory.size() + all_refs.size());

            for (const auto& sf : rules_for_inventory) {
                inventory.push_back({sf.path.filename().string(),
                                     sf.scope_label, sf.mtime, sf.tags});
            }
            for (const auto& r : all_refs) {
                inventory.push_back({r.path.filename().string(),
                                     r.scope_label, r.mtime, r.tags});
            }

            // Brainstorm-mode scribe cross-vault: list rules + refs in
            // teammate vaults the scribe can cross-write into. Teammate
            // vaults are sibling dirs of `vault_dir` under <base>/vaults/.
            // Only filenames + mtimes are read (no content) — inventory is
            // metadata-only.
            if (agent_role == "scribe" && conversation_mode == "brainstorm") {
                auto vault_path = std::filesystem::path(vault_dir);
                auto vaults_root = vault_path.parent_path();
                if (std::filesystem::exists(vaults_root) &&
                    std::filesystem::is_directory(vaults_root)) {
                    for (const auto& sib : std::filesystem::directory_iterator(vaults_root)) {
                        if (!sib.is_directory()) continue;
                        auto other_name = sib.path().filename().string();
                        if (other_name == agent_name) continue;
                        auto other_knowledge = sib.path() / "knowledge";
                        if (!std::filesystem::exists(other_knowledge) ||
                            !std::filesystem::is_directory(other_knowledge)) continue;
                        std::string other_label = "vault: " + other_name;
                        for (const auto& kentry : std::filesystem::directory_iterator(other_knowledge)) {
                            if (!kentry.is_regular_file()) continue;
                            auto kind = classify_knowledge_filename(
                                kentry.path().filename().string());
                            if (kind != KnowledgeKind::Rule &&
                                kind != KnowledgeKind::Reference) continue;
                            // Cross-vault entries are metadata-only (no
                            // content read), so tags stay empty — the
                            // emit loop will skip the [tags] bracket for
                            // these lines.
                            inventory.push_back({kentry.path().filename().string(),
                                                 other_label,
                                                 kentry.last_write_time(),
                                                 {}});
                        }
                    }
                }
            }

            if (!inventory.empty()) {
                std::sort(inventory.begin(), inventory.end(),
                          [](const InventoryEntry& a, const InventoryEntry& b) {
                              return a.mtime > b.mtime;
                          });

                constexpr size_t kInventoryCap = 50;
                size_t inv_evicted = 0;
                if (inventory.size() > kInventoryCap) {
                    inv_evicted = inventory.size() - kInventoryCap;
                    inventory.resize(kInventoryCap);
                }

                prompt += "## Vault Inventory\n\n";
                prompt += "Knowledge files in your scope. Use this list to "
                          "decide whether a VAULT_UPDATE should target an "
                          "existing file or create a new one.\n\n";
                for (const auto& e : inventory) {
                    prompt += "- " + e.filename;
                    // Phase 9 Track 3: render tags as `[a, b, c]` between
                    // filename and scope. Brackets are omitted entirely
                    // when the file has no tags so untagged lines stay
                    // visually clean (and so cross-vault metadata-only
                    // entries don't show a phantom empty bracket pair).
                    if (!e.tags.empty()) {
                        prompt += " [";
                        for (size_t i = 0; i < e.tags.size(); ++i) {
                            if (i > 0) prompt += ", ";
                            prompt += e.tags[i];
                        }
                        prompt += "]";
                    }
                    prompt += " (" + e.scope_label + ") — " +
                              format_human_mtime(e.mtime) + "\n";
                }
                if (inv_evicted > 0) {
                    prompt += "\n[" + std::to_string(inv_evicted) +
                              " additional files omitted from inventory — "
                              "most-recent " + std::to_string(kInventoryCap) +
                              " listed]\n";
                }
                prompt += "\n";
            }
        }

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

        // Append task — variable per turn, lives in user_message.
        prompt += "---\n\n";
        prompt += "# Current Task\n\n";
        prompt += "**Task type:** " + task_type + "\n";
        prompt += "**Agent:** " + agent_name + "\n\n";
        prompt += task_description;
        prompt += "\n";

        // Legacy output rules — stable identity, always emitted in
        // system_prompt regardless of team mode. The legacy assemble() shim
        // (concatenated as system_prompt + "\n---\n\n" + user_message)
        // preserves the historical byte layout that the team-mode path
        // previously depended on (the rules block was suppressed for team
        // mode in the single-string flow). In split mode the rules become
        // unconditional because they're identity, not per-turn instruction.
        system_prompt += "---\n\n";
        system_prompt += "# CRITICAL — Output Rules\n\n";
        system_prompt += "You MUST follow these rules for ALL output:\n\n";
        system_prompt += "1. **NEVER write files directly.** Do not use Write, Edit, or any file-creation tool. ";
        system_prompt += "All output goes in your response text as structured blocks.\n";
        system_prompt += "2. **NEVER run commands that modify files.** You may READ files and RUN queries ";
        system_prompt += "(sqlite3, cat, ls, grep), but never write, move, or delete.\n";
        system_prompt += "3. **ALL findings must use structured blocks** in your response: ";
        system_prompt += "VAULT_UPDATE, OBSERVATION, PROPOSAL, SUMMARY.\n";
        system_prompt += "4. **Only write to YOUR vault.** VAULT_UPDATE paths must start with `knowledge/` or `inbox/`.\n\n";
        system_prompt += "The daemon extracts these blocks from your response text and routes them. ";
        system_prompt += "If you write files directly, the daemon cannot track your output.\n\n";

        system_prompt += "---\n\n";
        system_prompt += "# Output Instructions\n\n";
        system_prompt += "When you have findings, use these structured blocks in your response:\n\n";
        system_prompt += "- **VAULT_UPDATE**: Your current distilled beliefs. Overwrites previous. Keep concise.\n";
        system_prompt += "- **OBSERVATION**: What you noticed. Timestamped, accumulated over time. Write freely.\n";
        system_prompt += "- **PROPOSAL**: Actions requiring consensus from other agents.\n\n";
        system_prompt += "```VAULT_UPDATE\n";
        system_prompt += "path: knowledge/<filename>.md\n";
        system_prompt += "content: |\n";
        system_prompt += "  <content to write>\n";
        system_prompt += "```\n\n";
        system_prompt += "```PROPOSAL\n";
        system_prompt += "title: <title>\n";
        system_prompt += "requires_consensus_from: [<agent_names>]\n";
        system_prompt += "content: |\n";
        system_prompt += "  <proposal details>\n";
        system_prompt += "```\n\n";
        system_prompt += "```OBSERVATION\n";
        system_prompt += "title: <what you observed>\n";
        system_prompt += "tags: [<relevant, topic, tags>]\n";
        system_prompt += "content: |\n";
        system_prompt += "  <detailed observation -- accumulated, never overwritten>\n";
        system_prompt += "```\n\n";
        system_prompt += "```SUMMARY\n";
        system_prompt += "<brief findings summary>\n";
        system_prompt += "```\n";

        return out;
    }

private:
    [[nodiscard]] static std::string read_file(const std::filesystem::path& path) {
        std::ifstream f(path);
        if (!f.is_open()) return {};
        return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    }

    // Phase 9 Track 1 — render a filesystem mtime as ISO-8601 UTC
    // (e.g. "2026-05-10T14:23:11Z"). Used by the ## Vault Inventory
    // section. clock_cast keeps the conversion portable across libc++ /
    // libstdc++; falls back to a now-anchored offset on platforms where
    // clock_cast is unavailable (older toolchains).
    [[nodiscard]] static std::string format_iso8601_utc(
        const std::filesystem::file_time_type& ft) {
        std::time_t t;
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
        auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
        t = std::chrono::system_clock::to_time_t(sys);
#else
        // Pre-C++20 fallback: anchor via now() delta. Loses no precision
        // for our purpose (seconds-resolution display).
        auto delta = ft - std::filesystem::file_time_type::clock::now();
        auto sys = std::chrono::system_clock::now() +
            std::chrono::duration_cast<std::chrono::system_clock::duration>(delta);
        t = std::chrono::system_clock::to_time_t(sys);
#endif
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return std::string(buf);
    }

    // Phase 9 Track 3 — render a filesystem mtime as a human-relative string
    // (e.g. "just now", "5m ago", "3h ago", "2d ago"). Used by the
    // ## Vault Inventory section so agents see recency at a glance without
    // parsing ISO timestamps. Negative deltas (future mtimes from clock
    // skew) clamp to "just now".
    [[nodiscard]] static std::string format_human_mtime(
        const std::filesystem::file_time_type& ft) {
        std::chrono::system_clock::time_point sys;
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
        sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
#else
        // Pre-C++20 fallback: anchor via now() delta (mirrors
        // format_iso8601_utc on older toolchains).
        auto delta = ft - std::filesystem::file_time_type::clock::now();
        sys = std::chrono::system_clock::now() +
            std::chrono::duration_cast<std::chrono::system_clock::duration>(delta);
#endif
        auto now = std::chrono::system_clock::now();
        auto delta_sec = std::chrono::duration_cast<std::chrono::seconds>(
            now - sys).count();
        if (delta_sec < 60) return "just now";
        auto delta_min = delta_sec / 60;
        if (delta_min < 60) return std::to_string(delta_min) + "m ago";
        auto delta_hr = delta_min / 60;
        if (delta_hr < 24) return std::to_string(delta_hr) + "h ago";
        auto delta_day = delta_hr / 24;
        return std::to_string(delta_day) + "d ago";
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
