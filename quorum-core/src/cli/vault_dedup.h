#pragma once

// Phase 10 Track 3 — `quorum vault dedup` CLI helpers.
//
// Greedy single-link clustering on Jaccard similarity of token sets
// (strip_frontmatter then tokenize_lower). Surfaces near-duplicate
// rule-*.md / ref-*.md files for operator review. Action mode is
// interactive (merge/delete/keep-all/skip); --dry-run lists clusters
// with similarities + excerpts and performs no mutation.
//
// Header-only, matches the cli/skills.h / cli/agent_create.h /
// cli/init.h / cli/benchmark.h convention.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "agent/context_assembler.h"   // detail::tokenize_lower, classify_knowledge_filename, KnowledgeKind
#include "utils/discover.h"            // discover_project_root
#include "utils/frontmatter.h"         // strip_frontmatter

namespace sui::quorum::cli {

struct VaultDedupOptions {
    std::string vault_path;       // resolved target dir; non-empty by the time we call run_vault_dedup
    bool dry_run = false;
    bool use_global = false;       // user passed --global (informational; resolved into vault_path before run)
    double threshold = 0.7;        // Jaccard threshold; --threshold flag overrides
};

struct DedupFile {
    std::filesystem::path path;
    std::string content;                       // full content (frontmatter NOT yet stripped — needed for excerpt formatting if frontmatter holds a title)
    std::unordered_set<std::string> tokens;    // tokenize_lower(strip_frontmatter(content)) — computed once per file
};

// Clustering output. anchor_index is an index into the files vector that the
// clusterer was given. members lists every other file in the cluster along
// with its Jaccard similarity to the anchor (NOT to other members — greedy
// single-link). Singleton clusters are NOT emitted.
struct DedupCluster {
    size_t anchor_index;
    std::vector<std::pair<size_t, double>> members;
};

namespace detail {

// Read a file fully into a std::string. Returns empty string on open failure.
// Mirrors the read_file helper in context_assembler.h.
[[nodiscard]] inline std::string read_file_string(const std::filesystem::path& p) {
    std::ifstream in(p);
    if (!in.is_open()) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Compute the token set for a file's content. Steps:
//   1. strip_frontmatter — drop the leading YAML block so tags don't dominate
//      the set (Phase 9 #27d / plan Q9).
//   2. tokenize_lower    — lowercase ASCII, alnum-run split, drop tokens <2
//      chars and the small stopword list. Reused from context_assembler.h
//      so dedup and scoring tokenize identically (plan Q3).
//   3. unique             — collapse to a set; Jaccard works on sets.
[[nodiscard]] inline std::unordered_set<std::string> token_set(const std::string& content) {
    auto stripped = sui::quorum::strip_frontmatter(content);
    auto tokens = sui::quorum::detail::tokenize_lower(stripped);
    return std::unordered_set<std::string>(tokens.begin(), tokens.end());
}

// Jaccard similarity: |A ∩ B| / |A ∪ B|. Edge cases:
//   - both empty → 1.0 (semantically identical; surfaces empty/empty pairs
//     for operator review).
//   - one empty, other non-empty → 0.0.
//   - identical sets → 1.0.
// Implementation iterates the smaller set for O(min(|A|, |B|)) average.
[[nodiscard]] inline double jaccard(
    const std::unordered_set<std::string>& a,
    const std::unordered_set<std::string>& b) {
    if (a.empty() && b.empty()) return 1.0;
    size_t intersect = 0;
    const auto& smaller = (a.size() < b.size()) ? a : b;
    const auto& larger  = (a.size() < b.size()) ? b : a;
    for (const auto& t : smaller) {
        if (larger.count(t)) ++intersect;
    }
    size_t uni = a.size() + b.size() - intersect;
    if (uni == 0) return 0.0;
    return static_cast<double>(intersect) / static_cast<double>(uni);
}

// Enumerate eligible files in `dir` (non-recursive). Eligible = rule-*.md or
// ref-*.md (via classify_knowledge_filename). For each file we compute the
// token set once. Sorted by filename so output is deterministic.
[[nodiscard]] inline std::vector<DedupFile> enumerate_vault(
    const std::filesystem::path& dir) {
    std::vector<DedupFile> out;
    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto fname = entry.path().filename().string();
        auto kind = sui::quorum::classify_knowledge_filename(fname);
        if (kind != sui::quorum::KnowledgeKind::Rule &&
            kind != sui::quorum::KnowledgeKind::Reference) continue;
        DedupFile f;
        f.path = entry.path();
        f.content = read_file_string(entry.path());
        f.tokens = token_set(f.content);
        out.push_back(std::move(f));
    }
    std::sort(out.begin(), out.end(),
        [](const DedupFile& a, const DedupFile& b) {
            return a.path.filename().string() < b.path.filename().string();
        });
    return out;
}

// Greedy single-link clustering. The first unassigned file in iteration order
// becomes a cluster's anchor; every later unassigned file with Jaccard >
// threshold (strictly greater, per plan #11) gets pulled into that cluster
// and marked assigned. Order-dependent: a file may land in an earlier
// cluster even if it's more similar to a later cluster's anchor. This is
// acceptable — dedup surfaces candidates for operator review, not a
// globally-optimal partition. Singleton clusters are NOT emitted.
[[nodiscard]] inline std::vector<DedupCluster> cluster_files(
    const std::vector<DedupFile>& files, double threshold) {
    std::vector<DedupCluster> clusters;
    std::vector<bool> assigned(files.size(), false);
    for (size_t i = 0; i < files.size(); ++i) {
        if (assigned[i]) continue;
        DedupCluster c;
        c.anchor_index = i;
        assigned[i] = true;
        for (size_t j = i + 1; j < files.size(); ++j) {
            if (assigned[j]) continue;
            double sim = jaccard(files[i].tokens, files[j].tokens);
            if (sim > threshold) {
                c.members.emplace_back(j, sim);
                assigned[j] = true;
            }
        }
        if (!c.members.empty()) clusters.push_back(std::move(c));
    }
    return clusters;
}

// Make an 80-char one-line excerpt of a file's body for cluster display.
// Strips frontmatter, optionally drops a leading H1 line, then collapses any
// whitespace run (incl. newlines) to a single space. Appends "..." on
// truncation. No surrounding quotes — caller wraps.
[[nodiscard]] inline std::string make_dedup_excerpt(const std::string& content,
                                                    size_t max_chars = 80) {
    auto body = sui::quorum::strip_frontmatter(content);
    if (!body.empty() && body[0] == '#') {
        auto lf = body.find('\n');
        if (lf != std::string::npos) body = body.substr(lf + 1);
    }
    std::string out;
    out.reserve(max_chars + 4);
    bool in_ws = true;  // skip leading whitespace
    for (char c : body) {
        bool is_ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (is_ws) {
            if (!in_ws && out.size() < max_chars) out.push_back(' ');
            in_ws = true;
        } else {
            if (out.size() >= max_chars) { out += "..."; break; }
            out.push_back(c);
            in_ws = false;
        }
    }
    return out;
}

// Render one cluster to `os`. Format matches plan Q5:
//
//   Cluster 1 (3 files, threshold 0.7):
//     rule-foo.md  [base]      "excerpt..."
//     rule-bar.md  sim 0.82    "excerpt..."
//     rule-baz.md  sim 0.73    "excerpt..."
//
// One-line summary plus indented per-file lines. `[base]` marks the anchor.
inline void print_cluster(std::ostream& os, size_t cluster_idx,
                          const std::vector<DedupFile>& files,
                          const DedupCluster& cluster, double threshold) {
    size_t total = 1 + cluster.members.size();
    os << "Cluster " << cluster_idx << " (" << total << " files, threshold "
       << threshold << "):\n";
    const auto& anchor = files[cluster.anchor_index];
    os << "  " << anchor.path.filename().string()
       << "  [base]      \"" << make_dedup_excerpt(anchor.content) << "\"\n";
    for (const auto& [idx, sim] : cluster.members) {
        char simbuf[16];
        std::snprintf(simbuf, sizeof(simbuf), "sim %.2f", sim);
        os << "  " << files[idx].path.filename().string()
           << "  " << simbuf << "    \"" << make_dedup_excerpt(files[idx].content) << "\"\n";
    }
}

// Interactive action selection. Returns the action plus, for delete/merge,
// the 1-based index the operator chose (0 if cancelled/invalid). Reads two
// lines from stdin at most: the action char and, when needed, the index.
//
// Cancellation paths (return Skip):
//   - empty input / EOF
//   - first char other than m/d/k/s
//   - delete/merge with 'a' or out-of-range or non-numeric index
//
// Default on Enter is `s` (skip) — the safe action.
enum class DedupAction { Skip, KeepAll, DeleteOne, MergePickWinner };

inline std::pair<DedupAction, size_t> prompt_cluster_action(size_t cluster_size) {
    std::cout << "[m]erge / [d]elete / [k]eep all / [s]kip [s]: " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return {DedupAction::Skip, 0};
    char c = 0;
    for (char x : line) {
        if (x != ' ' && x != '\t') {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(x)));
            break;
        }
    }
    if (c == 0 || c == 's') return {DedupAction::Skip, 0};
    if (c == 'k') return {DedupAction::KeepAll, 0};
    if (c == 'd' || c == 'm') {
        std::cout << (c == 'd' ? "Which file to delete? "
                               : "Which file to keep (others will be deleted)? ")
                  << "Enter index (1-" << cluster_size << ") or \"a\" to cancel: "
                  << std::flush;
        std::string num_line;
        if (!std::getline(std::cin, num_line)) return {DedupAction::Skip, 0};
        for (char x : num_line) {
            if (x == 'a' || x == 'A') return {DedupAction::Skip, 0};
        }
        try {
            int idx = std::stoi(num_line);
            if (idx < 1 || static_cast<size_t>(idx) > cluster_size) {
                return {DedupAction::Skip, 0};
            }
            return {(c == 'd' ? DedupAction::DeleteOne : DedupAction::MergePickWinner),
                    static_cast<size_t>(idx)};
        } catch (...) {
            return {DedupAction::Skip, 0};
        }
    }
    return {DedupAction::Skip, 0};
}

// Apply an action to one cluster. The 1-based picked index addresses the
// flat list [anchor, members[0], members[1], ...]. Merge is "pick-winner":
// keep one file untouched and delete every other file in the cluster. We do
// NOT auto-concatenate contents (would corrupt frontmatter and headings);
// operator can use `k` (keep all) and hand-edit if they want merged content.
inline void apply_action(const std::vector<DedupFile>& files,
                         const DedupCluster& cluster,
                         DedupAction action, size_t picked_1based) {
    std::vector<size_t> all = {cluster.anchor_index};
    for (const auto& [idx, sim] : cluster.members) all.push_back(idx);
    auto pick = [&](size_t one_based) -> std::filesystem::path {
        return files[all[one_based - 1]].path;
    };
    switch (action) {
        case DedupAction::Skip:
            std::cout << "  -> skipped\n";
            break;
        case DedupAction::KeepAll:
            std::cout << "  -> kept " << all.size() << "\n";
            break;
        case DedupAction::DeleteOne: {
            auto p = pick(picked_1based);
            std::error_code ec;
            std::filesystem::remove(p, ec);
            if (ec) {
                std::cout << "  -> ERROR deleting " << p.filename().string()
                          << ": " << ec.message() << "\n";
            } else {
                std::cout << "  -> deleted: " << p.filename().string() << "\n";
            }
            break;
        }
        case DedupAction::MergePickWinner: {
            auto winner = pick(picked_1based);
            size_t deleted = 0;
            for (size_t i = 1; i <= all.size(); ++i) {
                if (i == picked_1based) continue;
                std::error_code ec;
                std::filesystem::remove(files[all[i - 1]].path, ec);
                if (!ec) ++deleted;
            }
            std::cout << "  -> merged into: " << winner.filename().string()
                      << " (deleted " << deleted << ")\n";
            break;
        }
    }
}

} // namespace detail

// Top-level entrypoint. Caller (main.cpp) is responsible for resolving
// opts.vault_path before calling — that is, walking --global / --vault /
// default-to-project-root logic and dropping a concrete directory in
// opts.vault_path. We just scan and act.
//
// Return codes:
//   0  — success (incl. "no files" / "no clusters", which are not errors)
//   1  — vault path missing / not a directory
[[nodiscard]] inline int run_vault_dedup(const VaultDedupOptions& opts) {
    namespace fs = std::filesystem;
    if (opts.vault_path.empty()) {
        std::cerr << "ERROR: vault dedup: empty vault path (internal error)\n";
        return 1;
    }
    fs::path vault(opts.vault_path);
    if (!fs::exists(vault) || !fs::is_directory(vault)) {
        std::cerr << "ERROR: vault path does not exist or is not a directory: "
                  << opts.vault_path << "\n";
        return 1;
    }

    auto files = detail::enumerate_vault(vault);
    if (files.empty()) {
        std::cout << "No rule-*.md or ref-*.md files found in " << opts.vault_path << "\n";
        return 0;
    }

    auto clusters = detail::cluster_files(files, opts.threshold);
    if (clusters.empty()) {
        std::cout << "No clusters at threshold " << opts.threshold
                  << " over " << files.size() << " file(s) scanned.\n";
        return 0;
    }

    if (opts.dry_run) {
        for (size_t i = 0; i < clusters.size(); ++i) {
            detail::print_cluster(std::cout, i + 1, files, clusters[i], opts.threshold);
            std::cout << "\n";
        }
        std::cout << "Found " << clusters.size() << " cluster(s) over "
                  << files.size() << " file(s) scanned.\n";
        return 0;
    }

    // Interactive mode
    for (size_t i = 0; i < clusters.size(); ++i) {
        detail::print_cluster(std::cout, i + 1, files, clusters[i], opts.threshold);
        size_t csize = 1 + clusters[i].members.size();
        auto [action, picked] = detail::prompt_cluster_action(csize);
        detail::apply_action(files, clusters[i], action, picked);
        std::cout << "\n";
    }
    return 0;
}

} // namespace sui::quorum::cli
