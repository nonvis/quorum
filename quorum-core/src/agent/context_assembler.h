#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace sui::quorum {

struct ContextBudget {
    size_t max_files = 20;
    size_t max_chars = 200000;  // rough proxy for tokens (~4 chars/token)
};

// Assembles context for agent invocation from vault contents + task description.
// Reads CONTEXT.md (always), then recent knowledge files up to budget.
class ContextAssembler {
public:
    // Build a prompt string from agent vault + task description.
    [[nodiscard]] std::string assemble(const std::string& agent_name,
                                        const std::string& vault_dir,
                                        const std::string& task_type,
                                        const std::string& task_description,
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

        // Load knowledge files (most recent first)
        auto knowledge_dir = std::filesystem::path(vault_dir) / "knowledge";
        if (std::filesystem::exists(knowledge_dir) && std::filesystem::is_directory(knowledge_dir)) {
            auto files = list_files_by_recency(knowledge_dir);
            for (const auto& f : files) {
                if (files_loaded >= budget.max_files) break;
                if (prompt.size() >= budget.max_chars) break;

                auto content = read_file(f);
                if (!content.empty()) {
                    prompt += "# Knowledge: " + f.filename().string() + "\n\n";
                    prompt += content;
                    prompt += "\n\n";
                    ++files_loaded;
                }
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

        // Append task
        prompt += "---\n\n";
        prompt += "# Current Task\n\n";
        prompt += "**Task type:** " + task_type + "\n";
        prompt += "**Agent:** " + agent_name + "\n\n";
        prompt += task_description;
        prompt += "\n\n";

        // Append output format instructions
        prompt += "---\n\n";
        prompt += "# Output Instructions\n\n";
        prompt += "When you have findings, use these structured blocks in your response:\n\n";
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
        prompt += "```SUMMARY\n";
        prompt += "<brief findings summary>\n";
        prompt += "```\n";

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
