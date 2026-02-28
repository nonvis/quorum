#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace sui::quorum::json {

// Extract a JSON string value by key: "key": "value"
inline std::optional<std::string> extract_string(const std::string& json, std::string_view key) {
    std::string search = "\"" + std::string(key) + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return std::nullopt;

    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return std::nullopt;
    ++pos;

    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r'))
        ++pos;

    if (pos >= json.size() || json[pos] != '"') return std::nullopt;
    ++pos;

    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            ++pos;
            switch (json[pos]) {
                case '"':  result += '"'; break;
                case '\\': result += '\\'; break;
                case 'n':  result += '\n'; break;
                case 't':  result += '\t'; break;
                case 'r':  result += '\r'; break;
                default:   result += json[pos]; break;
            }
        } else {
            result += json[pos];
        }
        ++pos;
    }
    return result;
}

// Extract a number value by key (handles both quoted and unquoted)
inline double extract_number(const std::string& json, std::string_view key, double fallback = 0.0) {
    std::string search = "\"" + std::string(key) + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return fallback;

    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return fallback;
    ++pos;

    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r'))
        ++pos;

    if (pos >= json.size()) return fallback;

    bool quoted = (json[pos] == '"');
    if (quoted) ++pos;

    try {
        return std::stod(json.substr(pos));
    } catch (...) {
        return fallback;
    }
}

// Extract an integer value by key
inline int64_t extract_int(const std::string& json, std::string_view key, int64_t fallback = 0) {
    std::string search = "\"" + std::string(key) + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return fallback;

    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return fallback;
    ++pos;

    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r'))
        ++pos;

    if (pos >= json.size()) return fallback;

    bool quoted = (json[pos] == '"');
    if (quoted) ++pos;

    try {
        return std::stoll(json.substr(pos));
    } catch (...) {
        return fallback;
    }
}

// Extract a boolean value by key
inline bool extract_bool(const std::string& json, std::string_view key, bool fallback = false) {
    std::string search = "\"" + std::string(key) + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return fallback;

    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return fallback;
    ++pos;

    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;

    if (pos >= json.size()) return fallback;

    if (json.compare(pos, 4, "true") == 0) return true;
    if (json.compare(pos, 5, "false") == 0) return false;
    return fallback;
}

// Build a JSON object string from key-value pairs
inline std::string build_object(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string result = "{";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) result += ",";
        result += "\"" + fields[i].first + "\":" + fields[i].second;
    }
    result += "}";
    return result;
}

// Quote a string value for JSON
inline std::string quote(std::string_view s) {
    std::string result = "\"";
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\t': result += "\\t"; break;
            case '\r': result += "\\r"; break;
            default:   result += c; break;
        }
    }
    result += "\"";
    return result;
}

// Extract the text content from an Anthropic Messages API response.
// Handles the standard single-text-block format:
//   {"content": [{"type": "text", "text": "..."}], "usage": {...}}
// Tool-use responses and multi-block responses are out of scope for Phase 1.
inline std::optional<std::string> extract_anthropic_content(const std::string& json) {
    // Find the "content" array
    auto content_pos = json.find("\"content\"");
    if (content_pos == std::string::npos) return std::nullopt;

    // Find the first '{' after "content" (start of first content block object)
    auto block_start = json.find('{', content_pos);
    if (block_start == std::string::npos) return std::nullopt;

    // Find the matching '}' for this block (handles nested quotes but not nested objects)
    auto block_end = json.find('}', block_start);
    if (block_end == std::string::npos) return std::nullopt;

    // Extract the block substring and find "text" key within it.
    // Because extract_string does a naive first-match, and "type":"text" appears
    // before the actual "text":"..." key, we need to skip past the "type" field.
    auto block = json.substr(block_start, block_end - block_start + 1);

    // Find "type" first, then search for "text" key after it
    auto type_pos = block.find("\"type\"");
    if (type_pos == std::string::npos) return std::nullopt;

    // Skip past the "type":"text" value — find the comma after it
    auto after_type = block.find(',', type_pos);
    if (after_type == std::string::npos) return std::nullopt;

    // Extract "text" from the remainder of the block
    auto remainder = block.substr(after_type);
    return extract_string(remainder, "text");
}

} // namespace sui::quorum::json
