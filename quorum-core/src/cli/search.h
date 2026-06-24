#pragma once

// Phase 15 — `quorum search "<query>" [--project <path|name>] [--agent <name>]
//             [--limit N] [--json]`.
//
// A DETERMINISTIC, no-LLM, read-only ranked keyword search over the project's
// accumulated knower-vault `ref-*.md` notes. It exposes the SAME scorer the
// daemon already uses internally to surface refs into an agent prompt
// (search_references() in agent/context_assembler.h) — score =
// fn_hits×3 + tag_hits×5 + content_hits, ordered by score DESC then mtime DESC.
//
// Unlike `quorum ask`, this command spends NO claude tokens: it loads the ref
// corpus from disk into RefEntry vectors and calls the pure scorer. The corpus
// is every `ref-*.md` under <root>/.quorum/vaults/<x>/knowledge/ (for EVERY
// subdir with a knowledge/ dir — not just the four known knowers, so custom /
// doer vaults are searchable) PLUS <root>/.quorum/knowledge/ref-*.md
// (project-scope promoted refs). `--agent <name>` restricts the corpus to that
// one vault.
//
// The pure entrypoint search_refs() is unit-tested directly
// (tests/unit/test_search.cpp); run_search() resolves the project root, prints
// the results (human or --json), and returns the exit code.
//
// Header-only, matches the cli/ask.h convention. Does NOT touch the daemon
// prompt-assembly path — it only #includes context_assembler.h to reuse the
// pure scorer + structs.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agent/context_assembler.h"   // RefEntry, ScoredRef, search_references
#include "utils/frontmatter.h"         // parse_frontmatter_tags/_field, strip_frontmatter
#include "utils/json.h"                // json::quote
#include "cli/ask.h"                   // resolve_project_path (shared --project semantics)
#include "utils/discover.h"            // discover_project_root

namespace sui::quorum::cli {

struct SearchOptions {
    std::string query;
    std::string project;   // path OR project name; empty = discover from cwd
    std::string agent;     // optional --agent <name>; empty = all vaults + project
    int limit = 10;
    bool json = false;
};

// One search hit returned by the pure search_refs() function. Carries the
// ScoredRef fields PLUS the resolved preview (the authored frontmatter
// `summary:` when present, else the scorer's body excerpt) so callers can
// render the preferred preview without re-reading the file.
struct SearchHit {
    std::filesystem::path path;
    std::string scope_label;
    int score = 0;
    std::filesystem::file_time_type mtime{};
    std::string preview;   // summary if non-empty, else ScoredRef.excerpt
};

namespace detail {

// Slurp a file fully into a string; empty string if absent / unreadable.
[[nodiscard]] inline std::string search_slurp(const std::filesystem::path& p) {
    std::ifstream in(p);
    if (!in.is_open()) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Load every ref-*.md under `knowledge_dir` into the `refs` vector, tagged with
// `scope_label`. Each RefEntry caches the parsed frontmatter tags, the authored
// summary, the frontmatter-stripped content (so tag words don't double-count in
// the content score), and the file mtime. Skips rule-*.md and non-ref files.
// Plain filesystem only; degrades silently when the dir is absent.
inline void load_refs_from_dir(const std::filesystem::path& knowledge_dir,
                               const std::string& scope_label,
                               std::vector<RefEntry>& refs) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(knowledge_dir, ec)) return;
    for (const auto& e : fs::directory_iterator(knowledge_dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file()) continue;
        const auto fn = e.path().filename().string();
        if (e.path().extension() != ".md") continue;
        if (fn.rfind("ref-", 0) != 0) continue;   // ref-* only; skip rule-*

        auto raw = search_slurp(e.path());
        std::error_code mec;
        auto mt = fs::last_write_time(e.path(), mec);

        RefEntry r;
        r.path = e.path();
        r.scope_label = scope_label;
        r.scope_rank = 0;
        r.tags = sui::quorum::parse_frontmatter_tags(raw);
        r.summary = sui::quorum::parse_frontmatter_field(raw, "summary");
        r.content = sui::quorum::strip_frontmatter(raw);
        r.mtime = mt;
        refs.push_back(std::move(r));
    }
}

// Resolve the ordered list of (knowledge_dir, scope_label) corpus sources for a
// search. When `agent` is non-empty, ONLY that vault's knowledge/ dir is used.
// Otherwise every subdir of .quorum/vaults/ that has a knowledge/ dir is
// included (custom/doer vaults too — not restricted to known knowers), plus the
// project-scope .quorum/knowledge/ dir. Vault subdirs are visited in sorted
// order for deterministic output. PURE: filesystem reads only.
[[nodiscard]] inline std::vector<std::pair<std::filesystem::path, std::string>>
resolve_corpus_dirs(const std::filesystem::path& project_root,
                    const std::string& agent) {
    namespace fs = std::filesystem;
    std::vector<std::pair<fs::path, std::string>> dirs;

    if (!agent.empty()) {
        auto kd = project_root / ".quorum" / "vaults" / agent / "knowledge";
        dirs.emplace_back(kd, "vault: " + agent);
        return dirs;
    }

    // All vault subdirs with a knowledge/ dir (sorted), then project-scope refs.
    auto vaults_root = project_root / ".quorum" / "vaults";
    std::error_code ec;
    if (fs::is_directory(vaults_root, ec)) {
        std::vector<std::string> names;
        for (const auto& sib : fs::directory_iterator(vaults_root, ec)) {
            if (ec) break;
            if (!sib.is_directory()) continue;
            if (fs::is_directory(sib.path() / "knowledge")) {
                names.push_back(sib.path().filename().string());
            }
        }
        std::sort(names.begin(), names.end());
        for (const auto& name : names) {
            dirs.emplace_back(vaults_root / name / "knowledge", "vault: " + name);
        }
    }
    dirs.emplace_back(project_root / ".quorum" / "knowledge", "project");
    return dirs;
}

}  // namespace detail

// PURE search entrypoint. Resolves the corpus, loads every ref into a RefEntry,
// scores them with the daemon's own search_references(), and returns up to
// opts.limit hits (score DESC, mtime DESC). Each hit's preview prefers the
// authored frontmatter `summary:` over the scorer's body excerpt. No
// filesystem writes, no claude. `searched_refs` / `searched_vaults` are
// out-params for the header line (count of refs loaded / corpus dirs that
// actually existed). PURE: filesystem reads only.
[[nodiscard]] inline std::vector<SearchHit> search_refs(
    const std::string& project_root, const SearchOptions& opts,
    size_t* searched_refs = nullptr, size_t* searched_vaults = nullptr) {
    namespace fs = std::filesystem;
    fs::path root(project_root);

    auto corpus = detail::resolve_corpus_dirs(root, opts.agent);

    std::vector<RefEntry> refs;
    // Map path → authored summary so we can re-prefer it on the ScoredRef side
    // (search_references already prefers summary in its excerpt, but it strips
    // to ~280 chars; we keep the raw summary for an explicit, lossless preview).
    std::unordered_map<std::string, std::string> path_summary;
    size_t vaults_seen = 0;

    for (const auto& [dir, label] : corpus) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        ++vaults_seen;
        size_t before = refs.size();
        detail::load_refs_from_dir(dir, label, refs);
        for (size_t i = before; i < refs.size(); ++i) {
            path_summary[refs[i].path.string()] = refs[i].summary;
        }
    }

    if (searched_refs) *searched_refs = refs.size();
    if (searched_vaults) *searched_vaults = vaults_seen;

    int limit = opts.limit > 0 ? opts.limit : 10;
    auto scored = search_references(refs, opts.query,
                                    static_cast<size_t>(limit));

    std::vector<SearchHit> hits;
    hits.reserve(scored.size());
    for (const auto& s : scored) {
        SearchHit h;
        h.path = s.path;
        h.scope_label = s.scope_label;
        h.score = s.score;
        h.mtime = s.mtime;
        auto it = path_summary.find(s.path.string());
        const std::string& summary =
            (it != path_summary.end()) ? it->second : std::string{};
        h.preview = summary.empty() ? s.excerpt : summary;
        hits.push_back(std::move(h));
    }
    return hits;
}

namespace detail {

// Trim leading/trailing ASCII whitespace from a copy.
[[nodiscard]] inline std::string trim_copy(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' ||
                     s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' ||
                     s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// Collapse newlines/tabs/runs of whitespace into single spaces for a one-line
// preview, then trim.
[[nodiscard]] inline std::string one_line(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool in_ws = false;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!in_ws && !out.empty()) { out.push_back(' '); in_ws = true; }
        } else {
            out.push_back(c);
            in_ws = false;
        }
    }
    return trim_copy(out);
}

// Format a file_time_type as a UTC YYYY-MM-DD date string. Best-effort: returns
// "????-??-??" on conversion failure.
[[nodiscard]] inline std::string format_date(
    std::filesystem::file_time_type mtime) {
    using namespace std::chrono;
    auto sctp = time_point_cast<system_clock::duration>(
        mtime - std::filesystem::file_time_type::clock::now() +
        system_clock::now());
    std::time_t tt = system_clock::to_time_t(sctp);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &tt);
#else
    gmtime_r(&tt, &tm_buf);
#endif
    char buf[16];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf) == 0) {
        return "\?\?\?\?-\?\?-\?\?";  // escaped to avoid trigraph warning
    }
    return std::string(buf);
}

}  // namespace detail

// Top-level entrypoint for `quorum search`. Resolves the project root (mirrors
// `ask`: --project as a path|name, else discover from cwd), runs the pure
// search, and prints results. Returns 0 on success, 1 on a resolution failure
// or an empty/whitespace query. A query that tokenizes to nothing (stopwords /
// sub-2-char tokens) or that matches no refs is NOT an error — it prints a
// clear message and returns 0 (never fabricates a hit).
[[nodiscard]] inline int run_search(const SearchOptions& opts) {
    // Empty/whitespace query → clear message, non-fatal-but-error exit (1) so
    // a script can tell "you forgot the query" from "zero matches".
    if (detail::trim_copy(opts.query).empty()) {
        std::cerr << "ERROR: search requires a query. "
                     "Usage: quorum search \"<query>\" "
                     "[--project <path|name>] [--agent <name>] "
                     "[--limit N] [--json]\n";
        return 1;
    }

    // Resolve project root (default cwd). --project may be a path or a name.
    std::string project_root;
    if (opts.project.empty()) {
        auto discovered = sui::quorum::discover_project_root();
        if (!discovered) {
            std::cerr << "ERROR: no .quorum/ found in current or parent "
                         "directories. Pass --project <path|name> or run from "
                         "inside a Quorum project.\n";
            return 1;
        }
        project_root = *discovered;
    } else {
        std::string err;
        project_root = resolve_project_path(opts.project, err);
        if (project_root.empty()) {
            std::cerr << "ERROR: " << err << "\n";
            return 1;
        }
    }

    size_t searched_refs = 0, searched_vaults = 0;
    auto hits = search_refs(project_root, opts, &searched_refs,
                            &searched_vaults);

    if (opts.json) {
        std::cout << "[";
        for (size_t i = 0; i < hits.size(); ++i) {
            const auto& h = hits[i];
            if (i) std::cout << ",";
            std::cout << "{"
                      << "\"path\":" << json::quote(h.path.string()) << ","
                      << "\"filename\":"
                      << json::quote(h.path.filename().string()) << ","
                      << "\"scope_label\":" << json::quote(h.scope_label) << ","
                      << "\"score\":" << h.score << ","
                      << "\"mtime\":"
                      << json::quote(detail::format_date(h.mtime)) << ","
                      << "\"preview\":"
                      << json::quote(detail::one_line(h.preview))
                      << "}";
        }
        std::cout << "]\n";
        return 0;
    }

    // Human output.
    if (hits.empty()) {
        std::cout << "No matching refs for \"" << opts.query
                  << "\" (searched " << searched_refs << " refs).\n";
        return 0;
    }

    std::cout << hits.size() << " result" << (hits.size() == 1 ? "" : "s")
              << " for \"" << opts.query << "\" — searched " << searched_refs
              << " ref" << (searched_refs == 1 ? "" : "s") << " across "
              << searched_vaults << " vault"
              << (searched_vaults == 1 ? "" : "s")
              << " (project: " << project_root << ")\n";
    for (const auto& h : hits) {
        std::cout << "  [score " << h.score << "] "
                  << h.path.filename().string() << "  ("
                  << h.scope_label << ")  "
                  << detail::format_date(h.mtime) << "\n";
        auto preview = detail::one_line(h.preview);
        if (!preview.empty()) {
            std::cout << "      " << preview << "\n";
        }
    }
    return 0;
}

}  // namespace sui::quorum::cli
