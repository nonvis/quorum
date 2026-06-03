#pragma once

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agent/output_parser.h"
#include "utils/config.h"

namespace sui::quorum {

// Manages per-agent vault directories on the local filesystem.
//
// Vault layout:
//   {base_dir}/vaults/{agent_id}/
//   ├── CONTEXT.md       (read-only, human-authored)
//   ├── knowledge/        (persistent agent findings)
//   ├── inbox/            (items from other agents / daemon)
//   ├── experiments/      (experiment designs and results)
//   └── decisions/        (past decisions linked to proposals)
//
// The context_assembler READS from vaults. This class handles WRITES —
// applying VaultUpdate blocks parsed from agent output.

// Result of classifying a VAULT_UPDATE path against the own-vault rule.
// (Phase 14 retired the brainstorm-mode scribe cross-write exception; writes
// are own-vault only in all modes.)
//
// In every accepted case, `target_agent` is the agent whose vault should
// receive the write and `relative_path` is the path inside that vault
// (always starting with "knowledge/" or "inbox/").
//
// When `accepted == false`, `reason` carries a human-readable explanation
// suitable for stderr logging; the caller should silently skip the write
// (do NOT crash the conversation) per the Phase 6 Track 3 contract.
struct VaultPathClassification {
    bool         accepted{false};
    std::string  target_agent;    // own agent_id, or another team member's id (cross-write)
    std::string  relative_path;   // path under target_agent's vault root
    bool         is_cross_vault{false};
    std::string  reason;          // populated when accepted == false
};

class VaultManager {
public:
    explicit VaultManager(std::string base_dir)
        : base_dir_(std::move(base_dir)) {}

    // Create the vault directory structure for an agent.
    // Returns true on success or if the structure already exists.
    [[nodiscard]] bool init_vault(std::string_view agent_id) const {
        auto root = vault_path(agent_id);
        auto knowledge = std::filesystem::path(root) / "knowledge";
        auto inbox     = std::filesystem::path(root) / "inbox";

        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        if (ec) {
            std::cerr << "vault_manager: failed to create " << root
                      << ": " << ec.message() << "\n";
            return false;
        }
        std::filesystem::create_directories(knowledge, ec);
        if (ec) {
            std::cerr << "vault_manager: failed to create " << knowledge.string()
                      << ": " << ec.message() << "\n";
            return false;
        }
        std::filesystem::create_directories(inbox, ec);
        if (ec) {
            std::cerr << "vault_manager: failed to create " << inbox.string()
                      << ": " << ec.message() << "\n";
            return false;
        }

        return true;
    }

    // Write a VaultUpdate to the agent's vault.
    // The update.path is relative to the vault root (e.g. "knowledge/analysis.md").
    // Creates parent directories if needed. Overwrites existing files.
    // Returns true on success.
    [[nodiscard]] bool apply_vault_update(std::string_view agent_id,
                                          const VaultUpdate& update) const {
        // Validate the relative path — reject any attempt to escape the vault
        if (!validate_relative_path(update.path)) {
            std::cerr << "vault_manager: rejected unsafe path: " << update.path << "\n";
            return false;
        }

        auto root = vault_path(agent_id);
        auto target = std::filesystem::path(root) / update.path;

        // Normalize and verify the resolved path is still under the vault root
        auto canonical_root = std::filesystem::weakly_canonical(root);
        auto canonical_target = std::filesystem::weakly_canonical(target);
        auto root_str = canonical_root.string();
        auto target_str = canonical_target.string();

        if (target_str.size() < root_str.size() ||
            target_str.substr(0, root_str.size()) != root_str) {
            std::cerr << "vault_manager: path escapes vault root: " << update.path << "\n";
            return false;
        }

        // Create parent directories if needed (e.g. knowledge/deep/)
        auto parent = target.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                std::cerr << "vault_manager: failed to create parent dirs for "
                          << target.string() << ": " << ec.message() << "\n";
                return false;
            }
        }

        // Write content
        std::ofstream out(target, std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "vault_manager: failed to open for writing: "
                      << target.string() << "\n";
            return false;
        }
        out << update.content;
        if (!out.good()) {
            std::cerr << "vault_manager: write error for: " << target.string() << "\n";
            return false;
        }

        return true;
    }

    // Apply multiple vault updates. Returns count of successful writes.
    [[nodiscard]] size_t apply_all_updates(std::string_view agent_id,
                                           const std::vector<VaultUpdate>& updates) const {
        size_t success_count = 0;
        for (const auto& update : updates) {
            if (apply_vault_update(agent_id, update)) {
                ++success_count;
            }
        }
        return success_count;
    }

    // ── Own-vault rule (Phase 6 Track 3 / Phase 14) ────────────────────────────

    // Pure classifier for VAULT_UPDATE paths.
    //
    // The own-vault rule (from the analyst CONTEXT.md prompt block):
    //   `path:` MUST start with `knowledge/` or `inbox/` and writes go to the
    //   EMITTING agent's vault. This holds in BOTH modes for every role.
    //
    // Phase 14 retired the scribe — the sole former exception to this rule. The
    // brainstorm accumulation doctrine is now "participating knowers self-write
    // their OWN lens's slice" (own-vault), so there is no cross-vault curator.
    // Any path that is NOT own-shape is rejected (the caller logs to stderr and
    // does NOT crash the conversation). `emitting_agent_role` and
    // `conversation_mode` are retained in the signature for the caller's
    // diagnostic logging but no longer gate acceptance.
    //
    // Pure function: no filesystem access, no I/O. Tested in
    // test_vault_update_brainstorm.cpp.
    [[nodiscard]] static VaultPathClassification classify_vault_path(
        std::string_view path,
        std::string_view emitting_agent_id,
        std::string_view emitting_agent_role,
        std::string_view conversation_mode,
        const std::vector<AgentMetadata>& team_agents)
    {
        (void)emitting_agent_role;
        (void)conversation_mode;
        (void)team_agents;
        VaultPathClassification c;

        if (path.empty()) {
            c.reason = "empty path";
            return c;
        }

        // Reject any path that fails directory-traversal safety up front.
        // Mirrors the safety net inside apply_vault_update; doing it here
        // gives a uniform reject-with-reason path for the caller's logs.
        if (!validate_relative_path(path)) {
            c.reason = "unsafe path (traversal or absolute)";
            return c;
        }

        // Own-vault shape: starts directly with knowledge/ or inbox/.
        const bool own_shape =
            (path.size() > 10 && path.substr(0, 10) == "knowledge/") ||
            (path.size() >  6 && path.substr(0,  6) == "inbox/");

        if (own_shape) {
            c.accepted       = true;
            c.target_agent   = std::string(emitting_agent_id);
            c.relative_path  = std::string(path);
            c.is_cross_vault = false;
            return c;
        }

        // Not own-shape — rejected for all roles in all modes. The scribe
        // cross-vault exception was retired in Phase 14.
        c.reason = "VAULT_UPDATE path must start with knowledge/ or inbox/ "
                   "(own-vault only; cross-vault writes are not permitted)";
        return c;
    }

    // Apply a VaultUpdate with conversation context.
    //
    // Combines `classify_vault_path()` with the existing per-agent write
    // primitive (`apply_vault_update`). On classifier rejection, a one-line
    // warning is emitted to stderr and the function returns false WITHOUT
    // throwing — this preserves the "don't crash the conversation" contract.
    //
    // The destination is `<vault-root>/<target_agent>/<relative_path>`. Phase
    // 14: target_agent is always emitting_agent_id (own-vault only; the scribe
    // cross-vault curator + its ref auto-promotion were retired). `project_root`
    // is retained in the signature for call-site compatibility but no longer
    // drives any promotion.
    [[nodiscard]] bool apply_vault_update_with_context(
        std::string_view emitting_agent_id,
        std::string_view emitting_agent_role,
        std::string_view conversation_mode,
        const std::vector<AgentMetadata>& team_agents,
        const VaultUpdate& update,
        std::string_view project_root = {}) const
    {
        (void)project_root;
        auto c = classify_vault_path(update.path, emitting_agent_id,
                                     emitting_agent_role, conversation_mode,
                                     team_agents);
        if (!c.accepted) {
            std::cerr << "vault_manager: skipped VAULT_UPDATE from "
                      << emitting_agent_id
                      << " (path='" << update.path
                      << "', mode=" << conversation_mode
                      << ", role=" << emitting_agent_role
                      << "): " << c.reason << "\n";
            return false;
        }

        VaultUpdate routed = update;
        routed.path = c.relative_path;
        return apply_vault_update(c.target_agent, routed);
    }

    // Apply multiple VaultUpdates with conversation context. Returns count of
    // successful writes (rejected updates are skipped, NOT counted).
    //
    // `project_root` is retained for call-site compatibility (Phase 14 retired
    // the ref auto-promotion path it used to drive).
    [[nodiscard]] size_t apply_all_updates_with_context(
        std::string_view emitting_agent_id,
        std::string_view emitting_agent_role,
        std::string_view conversation_mode,
        const std::vector<AgentMetadata>& team_agents,
        const std::vector<VaultUpdate>& updates,
        std::string_view project_root = {}) const
    {
        size_t success_count = 0;
        for (const auto& update : updates) {
            if (apply_vault_update_with_context(emitting_agent_id,
                                                emitting_agent_role,
                                                conversation_mode,
                                                team_agents, update,
                                                project_root)) {
                ++success_count;
            }
        }
        return success_count;
    }

    // Read a file from an agent's vault.
    // relative_path is relative to the vault root (e.g. "knowledge/analysis.md").
    // Returns nullopt if file doesn't exist or can't be read.
    [[nodiscard]] std::optional<std::string> read_file(std::string_view agent_id,
                                                        std::string_view relative_path) const {
        if (!validate_relative_path(relative_path)) {
            return std::nullopt;
        }

        auto target = std::filesystem::path(vault_path(agent_id)) / std::string(relative_path);

        std::ifstream in(target);
        if (!in.is_open()) return std::nullopt;

        std::string content{std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>()};
        return content;
    }

    // List filenames in a subdirectory of an agent's vault.
    // subdir is relative to vault root (e.g. "knowledge", "inbox").
    // Returns sorted filenames (not full paths). Non-recursive.
    [[nodiscard]] std::vector<std::string> list_files(std::string_view agent_id,
                                                       std::string_view subdir) const {
        std::vector<std::string> result;

        if (!validate_relative_path(subdir)) {
            return result;
        }

        auto dir = std::filesystem::path(vault_path(agent_id)) / std::string(subdir);

        std::error_code ec;
        if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
            return result;
        }

        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            if (entry.is_regular_file()) {
                result.push_back(entry.path().filename().string());
            }
        }

        std::sort(result.begin(), result.end());
        return result;
    }

    // Returns the full vault root path for an agent.
    [[nodiscard]] std::string vault_path(std::string_view agent_id) const {
        auto p = std::filesystem::path(base_dir_) / "vaults" / std::string(agent_id);
        return p.string();
    }

    // Returns true if the vault directory exists for the given agent.
    [[nodiscard]] bool exists(std::string_view agent_id) const {
        std::error_code ec;
        return std::filesystem::exists(vault_path(agent_id), ec);
    }

private:
    std::string base_dir_;  // e.g. "./data"

    // Validate a relative path to prevent directory traversal attacks.
    // Rejects paths containing ".." or starting with "/".
    [[nodiscard]] static bool validate_relative_path(std::string_view path) {
        if (path.empty()) return false;

        // Reject absolute paths
        if (path.front() == '/') return false;

        // Reject paths containing ".." anywhere
        // Check for: ".." at start, "/..", "../", or exact match ".."
        if (path == "..") return false;
        if (path.size() >= 2 && path.substr(0, 2) == "..") {
            if (path.size() == 2 || path[2] == '/') return false;
        }

        // Search for "/.." pattern
        auto pos = path.find("/..");
        while (pos != std::string_view::npos) {
            auto after = pos + 3;
            if (after == path.size() || path[after] == '/') return false;
            pos = path.find("/..", pos + 1);
        }

        // Reject paths with null bytes
        if (path.find('\0') != std::string_view::npos) return false;

        return true;
    }
};

} // namespace sui::quorum
