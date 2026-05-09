#pragma once

// daemon/phase_plan_checkoff.h
//
// Deterministic backstop for the scribe's phase-plan checkoff. The scribe
// agent is supposed to flip `- [ ] Task N: ...` to `- [x] Task N: ... (date)`
// when a conversation cycle finishes, but LLM-driven steps drift. This helper
// runs at conversation completion: it pulls every task prompt for the
// conversation out of the DB, regex-extracts task numbers, and rewrites the
// matching plan-file lines in-place via an atomic tmp+rename.
//
// Failure mode is silent — any IO/parse/regex error returns 0 and the
// conversation completion path is unaffected.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "storage/database.h"

namespace sui::quorum {

namespace detail {

// Read the first non-empty, non-comment line of the pointer file.
// Returns empty string on any failure.
inline std::string read_phase_plan_path(const std::filesystem::path& pointer_path) {
    std::ifstream f(pointer_path);
    if (!f.is_open()) return {};
    std::string line;
    while (std::getline(f, line)) {
        // Trim leading whitespace for empty/comment check
        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) continue;            // blank line
        if (line[i] == '#') continue;              // comment
        // Strip trailing whitespace including \r
        size_t end = line.size();
        while (end > i && (line[end - 1] == ' ' || line[end - 1] == '\t' ||
                            line[end - 1] == '\r' || line[end - 1] == '\n')) {
            --end;
        }
        return line.substr(i, end - i);
    }
    return {};
}

// Pull every prompt for the conversation and extract Task numbers from them.
inline std::set<int> extract_task_numbers(Database& db, int64_t conversation_id) {
    std::set<int> nums;

    std::vector<std::string> prompts;
    db.query(
        "SELECT prompt FROM tasks WHERE conversation_id = ?",
        [&](sqlite3_stmt* stmt) {
            sqlite3_bind_int64(stmt, 1, conversation_id);
        },
        [&](sqlite3_stmt* stmt) {
            auto p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (p) prompts.emplace_back(p);
        }
    );

    // Two regexes covering both phrasings:
    //   "Task 4:" / "Task 4 " (handoff prefix from leader)
    //   "#7"                  (compact form in plan files / older prompts)
    static const std::regex re_task(R"(Task\s+(\d+)[:\s])");
    static const std::regex re_hash(R"(#(\d+)\b)");

    for (const auto& p : prompts) {
        try {
            for (auto it = std::sregex_iterator(p.begin(), p.end(), re_task);
                 it != std::sregex_iterator(); ++it) {
                try { nums.insert(std::stoi((*it)[1].str())); } catch (...) {}
            }
            for (auto it = std::sregex_iterator(p.begin(), p.end(), re_hash);
                 it != std::sregex_iterator(); ++it) {
                try { nums.insert(std::stoi((*it)[1].str())); } catch (...) {}
            }
        } catch (...) {
            // Regex/conversion failure for this prompt — skip it, keep going.
        }
    }
    return nums;
}

// Rewrite a single plan line: `- [ ] ...` -> `- [x] ... (today)` if no date
// suffix is already present. Returns true if the line was modified.
inline bool flip_checkbox_line(std::string& line, const std::string& today_iso) {
    // Replace the first `[ ]` in the line with `[x]`. The matching regex below
    // only fires on lines that start with `- [ ]` (after optional indent), so a
    // simple find()-replace is safe here.
    auto pos = line.find("[ ]");
    if (pos == std::string::npos) return false;
    line.replace(pos, 3, "[x]");

    // Append date if not already present. Accept any (YYYY-MM-DD) anywhere in
    // the line as evidence a date is already there (idempotency).
    static const std::regex re_date(R"(\(\d{4}-\d{2}-\d{2}\))");
    if (!std::regex_search(line, re_date)) {
        // Trim trailing whitespace before appending.
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        line += " (" + today_iso + ")";
    }
    return true;
}

// Write file atomically: write to tmp, rename over destination.
inline bool atomic_write(const std::filesystem::path& dst,
                         const std::string& contents) {
    namespace fs = std::filesystem;
    auto tmp = dst;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
        if (!out.is_open()) return false;
        out << contents;
        if (!out.good()) return false;
    }
    std::error_code ec;
    fs::rename(tmp, dst, ec);
    if (ec) {
        // Best-effort cleanup; ignore errors.
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace detail

// Returns count of lines updated. Silent no-op (returns 0) on any error
// (missing pointer file, missing plan file, no Task N: prefix found).
inline int checkoff_completed_tasks(Database& db,
                                     int64_t conversation_id,
                                     const std::string& target_dir,
                                     const std::string& today_iso) {
    namespace fs = std::filesystem;
    try {
        if (target_dir.empty()) return 0;

        // 1. Resolve pointer file
        fs::path tdir(target_dir);
        auto pointer = tdir / ".quorum" / "current_phase.md";
        std::error_code ec;
        if (!fs::exists(pointer, ec) || ec) return 0;

        auto plan_path_str = detail::read_phase_plan_path(pointer);
        if (plan_path_str.empty()) return 0;

        // 2. Resolve plan path (relative to target_dir if not absolute)
        fs::path plan_path(plan_path_str);
        if (plan_path.is_relative()) {
            plan_path = tdir / plan_path;
        }
        if (!fs::exists(plan_path, ec) || ec) return 0;

        // 3. Read all task prompts; extract task numbers
        auto nums = detail::extract_task_numbers(db, conversation_id);
        if (nums.empty()) return 0;

        // 4. Read plan file
        std::ifstream in(plan_path);
        if (!in.is_open()) return 0;
        std::vector<std::string> lines;
        {
            std::string line;
            while (std::getline(in, line)) lines.push_back(std::move(line));
        }
        in.close();

        // 5. For each task number, find first matching unchecked line and flip
        int updated = 0;
        for (int n : nums) {
            std::regex re_task(R"_(^(\s*)-\s*\[\s*\]\s*Task\s+)_" +
                               std::to_string(n) + R"_([:\s].*$)_");
            std::regex re_hash(R"_(^(\s*)-\s*\[\s*\]\s*#)_" +
                               std::to_string(n) + R"_(\b.*$)_");
            for (auto& line : lines) {
                bool match = false;
                try {
                    match = std::regex_match(line, re_task) ||
                            std::regex_match(line, re_hash);
                } catch (...) {
                    match = false;
                }
                if (!match) continue;
                if (detail::flip_checkbox_line(line, today_iso)) {
                    ++updated;
                }
                break;  // only first matching line per task number
            }
        }

        if (updated == 0) return 0;

        // 6. Atomically write back
        std::ostringstream oss;
        for (size_t i = 0; i < lines.size(); ++i) {
            oss << lines[i];
            if (i + 1 < lines.size() || lines.size() == 0) oss << '\n';
            else oss << '\n';  // preserve trailing newline
        }
        if (!detail::atomic_write(plan_path, oss.str())) {
            std::cerr << "[checkoff] WARN: atomic write failed for "
                      << plan_path.string() << "\n";
            return 0;
        }
        return updated;
    } catch (const std::exception& e) {
        std::cerr << "[checkoff] WARN: " << e.what() << "\n";
        return 0;
    } catch (...) {
        std::cerr << "[checkoff] WARN: unknown error\n";
        return 0;
    }
}

} // namespace sui::quorum
