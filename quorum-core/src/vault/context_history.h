#pragma once

// vault/context_history.h
//
// Audit trail for CONTEXT.md rewrites. Every overwrite of an agent's
// CONTEXT.md (CLI: agent_create / agent_modify --regenerate, web: PUT
// /api/agents/:id/context) appends the prior content to a sibling
// .history file before truncating + writing the new content.
//
// Records are separated by a strong sentinel line:
//
//     ---QUORUM-HISTORY---
//     ## <ISO8601 UTC timestamp>
//
//     <prior content of CONTEXT.md>
//
// The sentinel is intentionally NOT a plain `---` because CONTEXT.md
// bodies legitimately contain markdown horizontal rules and YAML
// frontmatter delimiters. `---QUORUM-HISTORY---` cannot collide with
// well-formed markdown.
//
// The .history file is capped at the most recent 20 records. On the
// first write to a previously-nonexistent CONTEXT.md no .history file
// is created — the prior content from that first read becomes the first
// .history entry on the *next* call.

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace sui::quorum::vault {

// Sentinel line that separates history records.
inline constexpr const char* kHistorySentinel = "---QUORUM-HISTORY---";

// ISO8601 UTC timestamp with seconds, no milliseconds (e.g. 2026-05-09T14:23:01Z).
inline std::string history_timestamp_iso() {
    auto t = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

// Read a file fully into a string. Returns empty string on failure.
inline std::string read_file_to_string(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return "";
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

// Split history file content into records on lines exactly equal to
// kHistorySentinel. Records do NOT include the sentinel line itself.
// The first chunk before the first sentinel (if any) is dropped — the
// file format always starts with a sentinel.
inline std::vector<std::string> split_history_records(const std::string& content) {
    std::vector<std::string> records;
    if (content.empty()) return records;

    const std::string sentinel = kHistorySentinel;
    std::string current;
    bool in_record = false;
    size_t pos = 0;

    while (pos <= content.size()) {
        size_t nl = content.find('\n', pos);
        std::string line = (nl == std::string::npos)
            ? content.substr(pos)
            : content.substr(pos, nl - pos);

        if (line == sentinel) {
            if (in_record) {
                records.push_back(current);
                current.clear();
            }
            in_record = true;
        } else if (in_record) {
            current += line;
            if (nl != std::string::npos) current += '\n';
        }

        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (in_record) {
        records.push_back(current);
    }
    return records;
}

// Append `prior_content` as a new history record to <context_path>.history,
// then trim to the most recent `max_entries` records.
inline void append_history_record(const std::string& context_path,
                                   const std::string& prior_content,
                                   const std::string& timestamp,
                                   size_t max_entries = 20) {
    auto history_path = context_path + ".history";

    // Build the new record body (without leading sentinel — we reassemble below).
    std::string new_record;
    new_record += "## " + timestamp + "\n\n";
    new_record += prior_content;
    if (new_record.empty() || new_record.back() != '\n') {
        new_record += '\n';
    }

    // Read existing records (if any), append the new one, trim.
    auto existing = read_file_to_string(history_path);
    auto records = split_history_records(existing);
    records.push_back(new_record);

    if (records.size() > max_entries) {
        size_t drop = records.size() - max_entries;
        records.erase(records.begin(), records.begin() + drop);
    }

    // Reassemble: every record is preceded by a sentinel line.
    std::ostringstream out;
    for (const auto& r : records) {
        out << kHistorySentinel << "\n" << r;
    }

    std::ofstream f(history_path, std::ios::trunc | std::ios::binary);
    f << out.str();
}

// Write `new_content` to `context_path`. If the file already exists, its
// prior content is appended to <context_path>.history (capped at the
// most recent 20 entries) before the truncate + write.
//
// First-write behavior: when context_path does not yet exist, no
// .history file is created. The fresh content becomes part of the
// audit trail on the *next* call (when it's the prior content).
inline void write_context_with_history(const std::string& context_path,
                                        const std::string& new_content) {
    namespace fs = std::filesystem;

    if (fs::exists(context_path)) {
        auto prior = read_file_to_string(context_path);
        // Only record if there was actual prior content to preserve.
        // (An empty file is still recorded — overwrite of an empty file
        // is still a state transition worth auditing.)
        append_history_record(context_path, prior, history_timestamp_iso());
    }

    // Ensure parent dir exists (defensive — callers usually create it).
    auto parent = fs::path(context_path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }

    std::ofstream f(context_path, std::ios::trunc | std::ios::binary);
    f << new_content;
}

} // namespace sui::quorum::vault
