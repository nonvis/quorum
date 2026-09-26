#pragma once

// Portable path references for the TRACKED files under .quorum/.
//
// Why this exists: `.quorum/agents/*.yaml` is committed with the project, but
// every writer used to store the absolute path it resolved on the machine that
// ran it (`/Users/<me>/.claude/skills/...`, `<quorum checkout>/templates/...`,
// `<project>` as a doer's target_dir). A clone on another machine then pointed
// at files that do not exist there, and the assembler quietly dropped the skill.
//
// A stored reference is one of:
//   ~/<rest>          under $HOME                (user-installed skills)
//   $QUORUM/<rest>    under the Quorum checkout  (knower lens skills, templates)
//   <relative>        under the project root     (target_dir `.`, project skills)
//   <absolute>        anything else — still accepted, never produced when one of
//                     the three forms above applies
//
// $QUORUM is the repo root of the RUNNING binary (`<repo>/build/quorum_daemon`,
// see utils/self_path.h), so a reference follows the checkout wherever it lives.
//
// The two-argument helpers take $HOME from the environment and $QUORUM from the
// running executable; the four-argument forms are PURE (no environment, no
// filesystem) for tests and callers that already hold the roots.

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include "utils/self_path.h"

namespace sui::quorum {

inline constexpr std::string_view kQuorumRootToken = "$QUORUM";

namespace detail {

// "<token>" -> "", "<token>/<rest>" -> "<rest>", anything else -> not a match.
inline bool split_ref_token(std::string_view ref, std::string_view token,
                            std::string_view& rest) {
    if (ref == token) {
        rest = {};
        return true;
    }
    if (ref.size() > token.size() && ref.starts_with(token) &&
        ref[token.size()] == '/') {
        rest = ref.substr(token.size() + 1);
        return true;
    }
    return false;
}

// Join and normalize, without the trailing separator lexically_normal leaves
// on "<root>/." (so target_dir "." expands to exactly the project root).
inline std::string join_normal(const std::string& root, std::string_view rest) {
    namespace fs = std::filesystem;
    auto s = (fs::path(root) / fs::path(std::string(rest))).lexically_normal().string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

inline std::string strip_trailing_slashes(std::string s) {
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

}  // namespace detail

// Expand a stored reference to a filesystem path. PURE.
// A token whose root is unknown (empty) is returned unchanged, so the caller's
// existence check fails loudly instead of resolving somewhere unintended.
[[nodiscard]] inline std::string expand_path_ref(std::string_view ref,
                                                 const std::string& project_root,
                                                 const std::string& quorum_root,
                                                 const std::string& home) {
    if (ref.empty()) return {};
    std::string_view rest;
    if (detail::split_ref_token(ref, "~", rest)) {
        return home.empty() ? std::string(ref) : detail::join_normal(home, rest);
    }
    if (detail::split_ref_token(ref, kQuorumRootToken, rest)) {
        return quorum_root.empty() ? std::string(ref)
                                   : detail::join_normal(quorum_root, rest);
    }
    if (std::filesystem::path(std::string(ref)).is_relative() && !project_root.empty()) {
        return detail::join_normal(project_root, ref);
    }
    return std::string(ref);
}

// The portable spelling of an ABSOLUTE path: project-relative, $QUORUM/..., or
// ~/..., whichever root contains it most specifically (longest root wins; on a
// tie the project beats $QUORUM beats ~). Relative input and paths outside all
// three roots are returned unchanged. PURE (lexical, no filesystem access).
[[nodiscard]] inline std::string portable_path_ref(const std::string& path,
                                                   const std::string& project_root,
                                                   const std::string& quorum_root,
                                                   const std::string& home) {
    namespace fs = std::filesystem;
    if (path.empty() || !fs::path(path).is_absolute()) return path;
    const fs::path p(detail::strip_trailing_slashes(fs::path(path).lexically_normal().string()));

    std::string best = path;
    size_t best_len = 0;
    auto consider = [&](const std::string& root_in, std::string_view token) {
        if (root_in.empty()) return;
        const auto root = detail::strip_trailing_slashes(
            fs::path(root_in).lexically_normal().string());
        if (!fs::path(root).is_absolute()) return;
        const auto rel = p.lexically_relative(root);
        if (rel.empty() || *rel.begin() == "..") return;
        if (root.size() <= best_len) return;  // a longer root already matched
        best_len = root.size();
        const bool at_root = (rel == ".");
        if (token.empty()) {
            best = at_root ? std::string(".") : rel.string();
        } else {
            best = std::string(token) + (at_root ? "" : "/" + rel.string());
        }
    };
    consider(project_root, "");
    consider(quorum_root, kQuorumRootToken);
    consider(home, "~");
    return best;
}

// $HOME, or "" when unset.
[[nodiscard]] inline std::string home_dir() {
    const char* h = std::getenv("HOME");
    return h ? std::string(h) : std::string{};
}

// The Quorum checkout the running binary was built from; "" if unknown.
[[nodiscard]] inline const std::string& running_quorum_root() {
    static const std::string root = quorum_repo_root_from_exe(nullptr);
    return root;
}

[[nodiscard]] inline std::string expand_path_ref(std::string_view ref,
                                                 const std::string& project_root) {
    return expand_path_ref(ref, project_root, running_quorum_root(), home_dir());
}

[[nodiscard]] inline std::string portable_path_ref(const std::string& path,
                                                   const std::string& project_root) {
    return portable_path_ref(path, project_root, running_quorum_root(), home_dir());
}

}  // namespace sui::quorum
