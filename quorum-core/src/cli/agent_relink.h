#pragma once

// `quorum agent relink [--dry-run]` — repair the machine-specific paths in a
// project's tracked `.quorum/agents/*.yaml`.
//
// Projects set up before utils/path_ref.h carry the absolute paths of the
// machine that ran `quorum init` / setup-knowers.sh. On any other machine
// those files do not exist and the agents run without their skills. relink
// rewrites each agent's `skill_file` and `executor.target_dir` into its
// portable spelling (~/, $QUORUM/, project-relative). A foreign absolute path
// that is missing here is re-rooted by its recognizable tail:
//   .../<project dir name>[/rest]  ->  .  or  <rest>
//   .../.claude/skills/<rest>      ->  ~/.claude/skills/<rest>
//   .../templates/skills/<rest>    ->  $QUORUM/templates/skills/<rest>
// A candidate is taken only if it resolves on THIS machine. Only those two
// value lines change; every other byte of the yaml is kept. --dry-run writes
// nothing. Exit 0 when every path resolves, 1 when any is left unresolved.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "utils/discover.h"
#include "utils/file_io.h"
#include "utils/path_ref.h"

namespace sui::quorum::cli {

struct RelinkEdit {
    std::string agent;
    std::string field;   // "skill_file" | "target_dir"
    std::string from;
    std::string to;      // == from when already portable or unresolved
    bool resolved = true;
};

struct RelinkPlan {
    std::vector<RelinkEdit> edits;                            // every path seen
    std::vector<std::pair<std::string, std::string>> writes;  // yaml path -> new text
};

// The portable ref for `ref` on this machine, or nullopt if nothing resolves.
[[nodiscard]] inline std::optional<std::string> relink_path_ref(
    const std::string& ref, const std::string& project_root,
    const std::string& quorum_root, const std::string& home) {
    namespace fs = std::filesystem;
    auto resolves = [&](const std::string& r) {
        std::error_code ec;
        return fs::exists(expand_path_ref(r, project_root, quorum_root, home), ec);
    };
    if (resolves(ref)) return portable_path_ref(ref, project_root, quorum_root, home);
    if (!fs::path(ref).is_absolute()) return std::nullopt;

    std::string path = ref;
    while (path.size() > 1 && path.back() == '/') path.pop_back();

    std::vector<std::string> candidates;
    const auto project_name = fs::path(project_root).filename().string();
    if (!project_name.empty()) {
        const auto tail = "/" + project_name;
        if (path.ends_with(tail)) candidates.push_back(".");
        auto pos = path.rfind(tail + "/");
        if (pos != std::string::npos) candidates.push_back(path.substr(pos + tail.size() + 1));
    }
    const std::pair<std::string, std::string> markers[] = {
        {"/.claude/skills/", "~/.claude/skills/"},
        {"/templates/skills/", std::string(kQuorumRootToken) + "/templates/skills/"},
    };
    for (const auto& [marker, replacement] : markers) {
        auto pos = path.rfind(marker);
        if (pos != std::string::npos) {
            candidates.push_back(replacement + path.substr(pos + marker.size()));
        }
    }
    for (const auto& c : candidates) {
        if (resolves(c)) return c;
    }
    return std::nullopt;
}

namespace detail {

inline std::string relink_trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

}  // namespace detail

// Plan the rewrite of every `.quorum/agents/*.yaml` under project_root. Reads
// the filesystem, writes nothing.
[[nodiscard]] inline RelinkPlan plan_relink(const std::string& project_root,
                                            const std::string& quorum_root,
                                            const std::string& home) {
    namespace fs = std::filesystem;
    RelinkPlan plan;
    const auto agents_dir = fs::path(project_root) / ".quorum" / "agents";
    std::error_code ec;
    if (!fs::is_directory(agents_dir, ec)) return plan;

    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(agents_dir, ec)) {
        if (entry.path().extension() == ".yaml") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());

    for (const auto& file : files) {
        const auto agent = file.stem().string();
        std::istringstream in(sui::quorum::detail::read_file_text(file));
        std::string out, line, section;
        bool changed = false;
        while (std::getline(in, line)) {
            const auto indent = line.find_first_not_of(' ');
            const auto colon = line.find(':');
            std::string field;
            if (indent != std::string::npos && colon != std::string::npos) {
                const auto key = detail::relink_trim(line.substr(0, colon));
                const auto val = detail::relink_trim(line.substr(colon + 1));
                if (indent == 0) section = val.empty() ? key : "";
                if (indent == 0 && key == "skill_file") field = "skill_file";
                if (indent > 0 && section == "executor" && key == "target_dir") field = "target_dir";
            }
            if (!field.empty()) {
                auto raw = detail::relink_trim(line.substr(colon + 1));
                char quote = 0;
                if (raw.size() >= 2 && (raw.front() == '"' || raw.front() == '\'') &&
                    raw.back() == raw.front()) {
                    quote = raw.front();
                    raw = raw.substr(1, raw.size() - 2);
                }
                RelinkEdit edit{agent, field, raw, raw, true};
                if (auto to = relink_path_ref(raw, project_root, quorum_root, home)) {
                    edit.to = *to;
                } else {
                    edit.resolved = false;
                }
                if (edit.to != edit.from) {
                    const auto value = quote ? std::string(1, quote) + edit.to + quote : edit.to;
                    line = line.substr(0, colon + 1) + " " + value;
                    changed = true;
                }
                plan.edits.push_back(std::move(edit));
            }
            out += line + "\n";
        }
        if (changed) plan.writes.emplace_back(file.string(), std::move(out));
    }
    return plan;
}

inline int run_agent_relink(bool dry_run) {
    auto project_root = sui::quorum::discover_project_root();
    if (!project_root) {
        std::cerr << "ERROR: no .quorum/ found. Run 'quorum init' first.\n";
        return 1;
    }
    const auto& quorum_root = sui::quorum::running_quorum_root();
    const auto home = sui::quorum::home_dir();
    auto plan = plan_relink(*project_root, quorum_root, home);

    std::cout << "Agent paths in " << *project_root << "/.quorum/agents/\n"
              << "  $QUORUM = " << (quorum_root.empty() ? "(unknown)" : quorum_root)
              << "   ~ = " << (home.empty() ? "(unset)" : home) << "\n";

    size_t rewritten = 0, kept = 0, unresolved = 0;
    for (const auto& e : plan.edits) {
        if (!e.resolved) {
            ++unresolved;
            std::cout << "\n  " << e.agent << "  " << e.field << "  " << e.from << "\n"
                      << "      UNRESOLVED: nothing matching exists on this machine; set it with\n"
                      << "      `quorum agent modify --name " << e.agent << " --"
                      << (e.field == "skill_file" ? "skill-file" : "target-dir") << " <path>`\n";
        } else if (e.to != e.from) {
            ++rewritten;
            std::cout << "\n  " << e.agent << "  " << e.field << "  " << e.from << "\n"
                      << "      -> " << e.to << "\n";
        } else {
            ++kept;
        }
    }
    std::cout << "\n" << plan.edits.size() << " paths: " << rewritten << " rewritten, "
              << kept << " already portable, " << unresolved << " unresolved\n";

    if (dry_run) {
        std::cout << "Dry run: nothing written.\n";
    } else {
        for (const auto& [path, text] : plan.writes) {
            std::ofstream out(path, std::ios::trunc);
            if (!out.is_open()) {
                std::cerr << "ERROR: cannot write " << path << "\n";
                return 1;
            }
            out << text;
        }
        std::cout << "Updated " << plan.writes.size() << " agent file(s).\n";
    }
    return unresolved == 0 ? 0 : 1;
}

}  // namespace sui::quorum::cli
