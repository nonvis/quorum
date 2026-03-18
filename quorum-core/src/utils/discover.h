#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;

namespace sui::quorum {

// Walk up from start_dir looking for .quorum/config.yaml.
// Returns absolute path to config.yaml, or nullopt if not found.
inline std::optional<std::string> discover_config(
    const std::string& start_dir = fs::current_path().string(),
    int max_depth = 20)
{
    auto dir = fs::absolute(fs::path(start_dir));
    for (int i = 0; i < max_depth; ++i) {
        auto candidate = dir / ".quorum" / "config.yaml";
        if (fs::exists(candidate)) {
            return candidate.string();
        }
        auto parent = dir.parent_path();
        if (parent == dir) break;  // filesystem root
        dir = parent;
    }
    return std::nullopt;
}

// Walk up from start_dir looking for a directory that contains .quorum/.
// Returns absolute path to that directory, or nullopt if not found.
inline std::optional<std::string> discover_project_root(
    const std::string& start_dir = fs::current_path().string(),
    int max_depth = 20)
{
    auto dir = fs::absolute(fs::path(start_dir));
    for (int i = 0; i < max_depth; ++i) {
        if (fs::exists(dir / ".quorum")) {
            return dir.string();
        }
        auto parent = dir.parent_path();
        if (parent == dir) break;  // filesystem root
        dir = parent;
    }
    return std::nullopt;
}

} // namespace sui::quorum
