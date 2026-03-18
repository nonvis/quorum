#pragma once

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace sui::quorum::cli {

struct SkillInfo {
    std::string id;    // directory name (e.g., "move-developer")
    std::string path;  // relative path to SKILL.md (e.g., ".claude/skills/move-developer/SKILL.md")
};

inline std::vector<SkillInfo> discover_skills(const std::string& project_root) {
    std::vector<SkillInfo> skills;
    auto skills_dir = fs::path(project_root) / ".claude" / "skills";
    if (!fs::exists(skills_dir) || !fs::is_directory(skills_dir)) {
        return skills;
    }
    for (const auto& entry : fs::directory_iterator(skills_dir)) {
        if (!entry.is_directory()) continue;
        auto skill_md = entry.path() / "SKILL.md";
        if (fs::exists(skill_md)) {
            skills.push_back(SkillInfo{
                entry.path().filename().string(),
                fs::relative(skill_md, project_root).string()
            });
        }
    }
    std::sort(skills.begin(), skills.end(),
              [](const SkillInfo& a, const SkillInfo& b) { return a.id < b.id; });
    return skills;
}

inline int list_skills(const std::string& project_root) {
    auto skills = discover_skills(project_root);
    if (skills.empty()) {
        std::cout << "No skills found in .claude/skills/\n";
        std::cout << "Claude Code skills live at .claude/skills/<name>/SKILL.md\n";
        return 0;
    }
    std::cout << "Skills (from .claude/skills/):\n";
    for (const auto& s : skills) {
        std::cout << "  " << s.id;
        // Pad to 25 chars
        if (s.id.size() < 25) {
            std::cout << std::string(25 - s.id.size(), ' ');
        } else {
            std::cout << "  ";
        }
        std::cout << s.path << "\n";
    }
    return 0;
}

} // namespace sui::quorum::cli
