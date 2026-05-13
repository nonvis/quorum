#pragma once

// Phase 9 Track 2 — minimal YAML-frontmatter tag extractor.
//
// Purpose: surface a knowledge file's `tags: [a, b, c]` frontmatter to the
// reference scorer so explicit tags carry weight (×5) above plain content
// matches. Independent of utils/config.h (Decision #10) — different schema,
// different fail-closed semantics, no shared state.
//
// Scope: single-line array form only. Multi-line YAML lists, nested keys,
// quoted strings, and escapes are out of scope. Any anomaly returns {}.
// Throws nothing.

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace sui::quorum {

[[nodiscard]] inline std::vector<std::string> parse_frontmatter_tags(
    const std::string& content) {
    const size_t n = content.size();
    size_t i = 0;

    // Skip leading whitespace (mirror make_excerpt's frontmatter detector).
    while (i < n && (content[i] == '\n' || content[i] == '\r' ||
                     content[i] == ' ' || content[i] == '\t')) {
        ++i;
    }

    // Require an opening '---' on its own line.
    if (i + 3 > n || content.compare(i, 3, "---") != 0) return {};
    if (i + 3 != n && content[i + 3] != '\n' && content[i + 3] != '\r') return {};
    size_t lf = content.find('\n', i);
    if (lf == std::string::npos) return {};
    size_t scan = lf + 1;

    // Walk lines until closing '---'. Within the block, look for `tags: [...]`.
    std::vector<std::string> tags;
    bool found_close = false;
    while (scan < n) {
        size_t end = content.find('\n', scan);
        std::string line = (end == std::string::npos)
            ? content.substr(scan)
            : content.substr(scan, end - scan);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line == "---") { found_close = true; break; }

        // Trim leading whitespace for prefix check.
        size_t s = 0;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        std::string trimmed = line.substr(s);
        constexpr const char* kKey = "tags:";
        if (trimmed.rfind(kKey, 0) == 0) {
            // Find single-line array form: tags: [a, b, c]
            size_t lb = trimmed.find('[', 5);
            size_t rb = trimmed.find(']', lb == std::string::npos ? 0 : lb);
            if (lb == std::string::npos || rb == std::string::npos) return {};
            std::string inner = trimmed.substr(lb + 1, rb - lb - 1);
            std::vector<std::string> parsed;
            std::string tok;
            auto flush = [&]() {
                size_t a = 0, b = tok.size();
                while (a < b && (tok[a] == ' ' || tok[a] == '\t')) ++a;
                while (b > a && (tok[b - 1] == ' ' || tok[b - 1] == '\t')) --b;
                if (b > a) {
                    std::string t = tok.substr(a, b - a);
                    std::transform(t.begin(), t.end(), t.begin(),
                        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                    parsed.push_back(std::move(t));
                }
                tok.clear();
            };
            for (char c : inner) {
                if (c == ',') flush();
                else tok.push_back(c);
            }
            flush();
            tags = std::move(parsed);
        }

        if (end == std::string::npos) return {};  // no closing '---'
        scan = end + 1;
    }

    if (!found_close) return {};
    return tags;
}

// Phase 10 Track 4 — extract a single scalar field from YAML frontmatter.
//
// Looks for a top-level line of the form:
//     <field_name>: <value>
//     <field_name>: "<value>"
// inside the leading `---` / `---` block (same block parse_frontmatter_tags
// reads). Returns the value with surrounding whitespace + at most one pair of
// double quotes stripped. Returns empty string when:
//   - no frontmatter block present
//   - frontmatter block unterminated
//   - field absent from frontmatter
//   - field present but value empty after trim
//
// Scope: single-line scalar values only. No multi-line strings, no nested
// keys, no anchors. Single-quoted values not supported (we never emit them).
// Matches the fail-open posture of parse_frontmatter_tags — any anomaly
// returns "" without throwing.
[[nodiscard]] inline std::string parse_frontmatter_field(
    const std::string& content, const std::string& field_name) {
    const size_t n = content.size();
    size_t i = 0;

    // Skip leading whitespace (mirror parse_frontmatter_tags).
    while (i < n && (content[i] == '\n' || content[i] == '\r' ||
                     content[i] == ' ' || content[i] == '\t')) {
        ++i;
    }

    if (i + 3 > n || content.compare(i, 3, "---") != 0) return {};
    if (i + 3 != n && content[i + 3] != '\n' && content[i + 3] != '\r') return {};
    size_t lf = content.find('\n', i);
    if (lf == std::string::npos) return {};
    size_t scan = lf + 1;

    const std::string key_prefix = field_name + ":";
    while (scan < n) {
        size_t end = content.find('\n', scan);
        std::string line = (end == std::string::npos)
            ? content.substr(scan)
            : content.substr(scan, end - scan);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line == "---") return {};

        // Trim leading whitespace for prefix check.
        size_t s = 0;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        std::string trimmed = line.substr(s);
        if (trimmed.rfind(key_prefix, 0) == 0) {
            std::string value = trimmed.substr(key_prefix.size());
            // trim leading
            size_t a = 0;
            while (a < value.size() && (value[a] == ' ' || value[a] == '\t')) ++a;
            // trim trailing
            size_t b = value.size();
            while (b > a && (value[b - 1] == ' ' || value[b - 1] == '\t')) --b;
            value = value.substr(a, b - a);
            // strip surrounding double quotes
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            return value;
        }

        if (end == std::string::npos) return {};  // unterminated
        scan = end + 1;
    }
    return {};
}

// Phase 9 finding #27d — strip leading YAML frontmatter so scoring code that
// reads file bodies doesn't double-count tag words (once as tag-hit ×5, once
// as content-hit ×1). Returns content unchanged if no frontmatter or if the
// opening `---` is unterminated (matches parse_frontmatter_tags's fail-open).
[[nodiscard]] inline std::string strip_frontmatter(const std::string& content) {
    const size_t n = content.size();
    size_t i = 0;
    while (i < n && (content[i] == '\n' || content[i] == '\r' ||
                     content[i] == ' ' || content[i] == '\t')) {
        ++i;
    }
    if (i + 3 > n || content.compare(i, 3, "---") != 0) return content;
    if (i + 3 != n && content[i + 3] != '\n' && content[i + 3] != '\r') return content;
    size_t lf = content.find('\n', i);
    if (lf == std::string::npos) return content;
    size_t scan = lf + 1;
    while (scan < n) {
        size_t end = content.find('\n', scan);
        std::string line = (end == std::string::npos)
            ? content.substr(scan)
            : content.substr(scan, end - scan);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "---") {
            return (end == std::string::npos) ? std::string{} : content.substr(end + 1);
        }
        if (end == std::string::npos) return content;  // unterminated
        scan = end + 1;
    }
    return content;
}

}  // namespace sui::quorum
