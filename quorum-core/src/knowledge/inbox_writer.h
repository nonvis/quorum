#pragma once

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "agent/output_parser.h"

namespace sui::quorum {

class InboxWriter {
public:
    explicit InboxWriter(const std::string& knowledge_dir)
        : knowledge_dir_(knowledge_dir) {}

    // Create the 3-zone directory structure: inbox/, library/, archive/
    [[nodiscard]] bool init() const {
        auto inbox   = std::filesystem::path(knowledge_dir_) / "inbox";
        auto library = std::filesystem::path(knowledge_dir_) / "library";
        auto archive = std::filesystem::path(knowledge_dir_) / "archive";

        std::error_code ec;
        std::filesystem::create_directories(inbox, ec);
        if (ec) {
            std::cerr << "inbox_writer: failed to create " << inbox.string()
                      << ": " << ec.message() << "\n";
            return false;
        }
        std::filesystem::create_directories(library, ec);
        if (ec) {
            std::cerr << "inbox_writer: failed to create " << library.string()
                      << ": " << ec.message() << "\n";
            return false;
        }
        std::filesystem::create_directories(archive, ec);
        if (ec) {
            std::cerr << "inbox_writer: failed to create " << archive.string()
                      << ": " << ec.message() << "\n";
            return false;
        }

        return true;
    }

    // Write a ParsedObservation to inbox/ as a markdown file.
    // Filename: {YYYY-MM-DD}_{HHMMSS}_{agent}_{task_type}.md
    // Returns true on success.
    [[nodiscard]] bool write_observation(const ParsedObservation& obs) const {
        auto [iso_ts, file_ts] = make_timestamps();

        auto safe_agent = sanitize(obs.agent);
        auto safe_type  = sanitize(obs.task_type);

        std::string filename = file_ts + "_" + safe_agent + "_" + safe_type + ".md";
        auto target = std::filesystem::path(knowledge_dir_) / "inbox" / filename;

        // Ensure parent directory exists
        auto parent = target.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                std::cerr << "inbox_writer: failed to create parent dirs for "
                          << target.string() << ": " << ec.message() << "\n";
                return false;
            }
        }

        // Build tags string: [tag1, tag2, tag3]
        std::string tags_str = "[";
        for (size_t i = 0; i < obs.tags.size(); ++i) {
            if (i > 0) tags_str += ", ";
            tags_str += obs.tags[i];
        }
        tags_str += "]";

        // Write file
        std::ofstream out(target, std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "inbox_writer: failed to open for writing: "
                      << target.string() << "\n";
            return false;
        }

        out << "---\n"
            << "agent: " << obs.agent << "\n"
            << "task_type: " << obs.task_type << "\n"
            << "date: " << iso_ts << "\n"
            << "tags: " << tags_str << "\n"
            << "processed: false\n"
            << "---\n"
            << "\n"
            << "# " << obs.title << "\n"
            << "\n"
            << obs.content << "\n";

        if (!out.good()) {
            std::cerr << "inbox_writer: write error for: " << target.string() << "\n";
            return false;
        }

        return true;
    }

    // Returns the inbox directory path
    [[nodiscard]] std::string inbox_dir() const {
        return (std::filesystem::path(knowledge_dir_) / "inbox").string();
    }

private:
    std::string knowledge_dir_;

    // Generate ISO timestamp and filename-safe timestamp.
    // Returns pair of {ISO timestamp for frontmatter, filename component}.
    [[nodiscard]] static std::pair<std::string, std::string> make_timestamps() {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);

        std::tm tm_buf{};
        gmtime_r(&time_t_now, &tm_buf);

        // ISO 8601: YYYY-MM-DDTHH:MM:SSZ
        std::ostringstream iso;
        iso << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");

        // Filename component: YYYY-MM-DD_HHMMSS
        std::ostringstream file;
        file << std::put_time(&tm_buf, "%Y-%m-%d_%H%M%S");

        return {iso.str(), file.str()};
    }

    // Sanitize a string for use in filenames: replace non-alphanumeric/non-underscore
    // characters with underscore. Returns "unknown" for empty input.
    [[nodiscard]] static std::string sanitize(const std::string& s) {
        if (s.empty()) return "unknown";
        std::string result;
        result.reserve(s.size());
        for (char c : s) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
                result += c;
            } else {
                result += '_';
            }
        }
        return result;
    }
};

} // namespace sui::quorum
