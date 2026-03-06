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
