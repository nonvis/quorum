#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace sui::quorum::json {

// Extract a JSON string value by key: "key": "value"
// Searches in a loop to skip matches where "key" appears as a VALUE rather than a KEY.
// A key is always followed (after optional whitespace) by ':'.
inline std::optional<std::string> extract_string(const std::string& json, std::string_view key) {
    std::string search = "\"" + std::string(key) + "\"";
    size_t search_from = 0;

    while (true) {
        auto pos = json.find(search, search_from);
        if (pos == std::string::npos) return std::nullopt;

        // Verify this is a KEY (next non-whitespace must be ':')
        auto after = pos + search.size();
        while (after < json.size() && (json[after] == ' ' || json[after] == '\t'
               || json[after] == '\n' || json[after] == '\r'))
            ++after;

        if (after >= json.size() || json[after] != ':') {
            // Not a key — it's a value. Skip and keep searching.
            search_from = pos + 1;
            continue;
        }

        // Found the key. Skip the ':' and extract the value.
        pos = after + 1;

        // Skip whitespace before value
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'
               || json[pos] == '\n' || json[pos] == '\r'))
            ++pos;

        if (pos >= json.size() || json[pos] != '"') return std::nullopt;
        ++pos;

        // Extract string value (handle escapes)
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
}

// Extract a number value by key (handles both quoted and unquoted)
// Searches in a loop to skip matches where "key" appears as a VALUE rather than a KEY.
inline double extract_number(const std::string& json, std::string_view key, double fallback = 0.0) {
    std::string search = "\"" + std::string(key) + "\"";
    size_t search_from = 0;

    while (true) {
        auto pos = json.find(search, search_from);
        if (pos == std::string::npos) return fallback;

        // Verify this is a KEY (next non-whitespace must be ':')
        auto after = pos + search.size();
        while (after < json.size() && (json[after] == ' ' || json[after] == '\t'
               || json[after] == '\n' || json[after] == '\r'))
            ++after;

        if (after >= json.size() || json[after] != ':') {
            search_from = pos + 1;
            continue;
        }

        // Found the key. Skip ':' and whitespace.
        pos = after + 1;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'
               || json[pos] == '\n' || json[pos] == '\r'))
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
}

// Extract an integer value by key
// Searches in a loop to skip matches where "key" appears as a VALUE rather than a KEY.
inline int64_t extract_int(const std::string& json, std::string_view key, int64_t fallback = 0) {
    std::string search = "\"" + std::string(key) + "\"";
    size_t search_from = 0;

    while (true) {
        auto pos = json.find(search, search_from);
        if (pos == std::string::npos) return fallback;

        // Verify this is a KEY (next non-whitespace must be ':')
        auto after = pos + search.size();
        while (after < json.size() && (json[after] == ' ' || json[after] == '\t'
               || json[after] == '\n' || json[after] == '\r'))
            ++after;

        if (after >= json.size() || json[after] != ':') {
            search_from = pos + 1;
            continue;
        }

        // Found the key. Skip ':' and whitespace.
        pos = after + 1;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'
               || json[pos] == '\n' || json[pos] == '\r'))
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
}

// Extract a boolean value by key
// Searches in a loop to skip matches where "key" appears as a VALUE rather than a KEY.
inline bool extract_bool(const std::string& json, std::string_view key, bool fallback = false) {
    std::string search = "\"" + std::string(key) + "\"";
    size_t search_from = 0;

    while (true) {
        auto pos = json.find(search, search_from);
        if (pos == std::string::npos) return fallback;

        // Verify this is a KEY (next non-whitespace must be ':')
        auto after = pos + search.size();
        while (after < json.size() && (json[after] == ' ' || json[after] == '\t'
               || json[after] == '\n' || json[after] == '\r'))
            ++after;

        if (after >= json.size() || json[after] != ':') {
            search_from = pos + 1;
            continue;
        }

        // Found the key. Skip ':' and whitespace.
        pos = after + 1;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'
               || json[pos] == '\n' || json[pos] == '\r'))
            ++pos;

        if (pos >= json.size()) return fallback;

        if (json.compare(pos, 4, "true") == 0) return true;
        if (json.compare(pos, 5, "false") == 0) return false;
        return fallback;
    }
}

// ─── Depth-0 ("top level") extraction ────────────────────────────────────────
//
// The extractors above are FLAT: they return the first `"<key>":` match at ANY
// nesting depth. That is wrong for the `claude -p --output-format json` result
// envelope. Claude Code 2.1.261 emits "usage".iterations[0]."type" == "message"
// at byte ~679, while the real top-level "type":"result" sits at byte ~1861 —
// so a flat read of "type" returns "message" and a healthy reply is rejected.
// Same hazard for "result"/"session_id"/"input_tokens": any nested namesake
// that precedes the top-level key shadows it.
//
// The functions below walk the document structurally (string-aware, escape-
// aware, brace/bracket depth-aware) and only ever see the ROOT object's own
// keys. Feed a nested object's raw text back in (see extract_top_level_object)
// to read that object's own depth-0 keys — e.g. usage.input_tokens without
// tripping over usage.iterations[].input_tokens.
//
// Manual parser: no library, no exceptions, C++20.
namespace detail {

inline constexpr int kMaxDepth = 128;
inline constexpr size_t kNpos = std::string::npos;

inline bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

inline bool is_scalar_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || c == '-' || c == '+' || c == '.';
}

inline size_t skip_ws(const std::string& s, size_t i) {
    while (i < s.size() && is_ws(s[i])) ++i;
    return i;
}

// s[i] must be '"'. Returns one past the closing quote, kNpos if unterminated.
// A backslash consumes the next byte, so \" and \\ never end the literal.
inline size_t skip_string(const std::string& s, size_t i) {
    if (i >= s.size() || s[i] != '"') return kNpos;
    ++i;
    while (i < s.size()) {
        if (s[i] == '\\') { i += 2; continue; }
        if (s[i] == '"') return i + 1;
        ++i;
    }
    return kNpos;
}

// Decode the string literal at s[i] == '"'. Escape handling is deliberately
// identical to extract_string() above (\uXXXX is NOT decoded; the 'u' is
// emitted literally) so both paths yield byte-identical text for a given value.
inline std::string decode_string(const std::string& s, size_t i) {
    std::string out;
    if (i >= s.size() || s[i] != '"') return out;
    ++i;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            switch (s[i]) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case 'n':  out += '\n'; break;
                case 't':  out += '\t'; break;
                case 'r':  out += '\r'; break;
                default:   out += s[i]; break;
            }
        } else {
            out += s[i];
        }
        ++i;
    }
    return out;
}

inline size_t skip_value(const std::string& s, size_t i, int depth);

// Strict object scan. s[i] must be '{'. Returns one past the matching '}', or
// kNpos if the region is not a syntactically well-formed JSON object.
// When `key` is non-empty and matches one of THIS object's own keys, the raw
// value bounds are written to *vb/*ve (first match wins). Callers must
// pre-seed *vb with kNpos.
inline size_t scan_object(const std::string& s, size_t i, int depth,
                          std::string_view key, size_t* vb, size_t* ve) {
    if (depth > kMaxDepth) return kNpos;
    if (i >= s.size() || s[i] != '{') return kNpos;
    i = skip_ws(s, i + 1);
    if (i < s.size() && s[i] == '}') return i + 1;
    while (true) {
        i = skip_ws(s, i);
        if (i >= s.size() || s[i] != '"') return kNpos;  // a key must be a string
        size_t key_begin = i;
        size_t key_end = skip_string(s, i);
        if (key_end == kNpos) return kNpos;
        i = skip_ws(s, key_end);
        if (i >= s.size() || s[i] != ':') return kNpos;
        i = skip_ws(s, i + 1);
        size_t val_begin = i;
        size_t val_end = skip_value(s, i, depth + 1);
        if (val_end == kNpos) return kNpos;
        if (vb && *vb == kNpos && !key.empty() && decode_string(s, key_begin) == key) {
            *vb = val_begin;
            *ve = val_end;
        }
        i = skip_ws(s, val_end);
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == '}') return i + 1;
        return kNpos;
    }
}

// s[i] must be '['. Returns one past the matching ']', kNpos if malformed.
inline size_t skip_array(const std::string& s, size_t i, int depth) {
    if (depth > kMaxDepth) return kNpos;
    if (i >= s.size() || s[i] != '[') return kNpos;
    i = skip_ws(s, i + 1);
    if (i < s.size() && s[i] == ']') return i + 1;
    while (true) {
        i = skip_ws(s, i);
        size_t e = skip_value(s, i, depth + 1);
        if (e == kNpos) return kNpos;
        i = skip_ws(s, e);
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == ']') return i + 1;
        return kNpos;
    }
}

// Returns one past the value starting at s[i], kNpos if malformed.
inline size_t skip_value(const std::string& s, size_t i, int depth) {
    if (depth > kMaxDepth || i >= s.size()) return kNpos;
    if (s[i] == '"') return skip_string(s, i);
    if (s[i] == '{') return scan_object(s, i, depth, {}, nullptr, nullptr);
    if (s[i] == '[') return skip_array(s, i, depth);
    size_t j = i;
    while (j < s.size() && is_scalar_char(s[j])) ++j;  // number / true / false / null
    return j > i ? j : kNpos;
}

// Locate the root object. `claude -p` runs with 2>&1, so stderr chatter can
// precede the envelope; the envelope is the value that ENDS the stream.
// Rule: the leftmost well-formed object followed only by whitespace wins;
// failing that, the leftmost well-formed object. A nested object can never
// end the stream (its parent's '}' follows it), so this never picks one.
inline bool find_root_object(const std::string& s, size_t* begin, size_t* end) {
    size_t fallback_b = kNpos, fallback_e = kNpos;
    for (size_t p = s.find('{'); p != kNpos; p = s.find('{', p + 1)) {
        size_t e = scan_object(s, p, 0, {}, nullptr, nullptr);
        if (e == kNpos) continue;
        if (skip_ws(s, e) >= s.size()) { *begin = p; *end = e; return true; }
        if (fallback_b == kNpos) { fallback_b = p; fallback_e = e; }
    }
    if (fallback_b == kNpos) return false;
    *begin = fallback_b;
    *end = fallback_e;
    return true;
}

}  // namespace detail

// Raw (undecoded) text of a top-level value: "abc" keeps its quotes, an object
// keeps its braces. nullopt when the document has no well-formed root object or
// the root object has no such key AT DEPTH 0.
inline std::optional<std::string> extract_top_level_raw(const std::string& json,
                                                        std::string_view key) {
    size_t b = 0, e = 0;
    if (!detail::find_root_object(json, &b, &e)) return std::nullopt;
    size_t vb = detail::kNpos, ve = detail::kNpos;
    if (detail::scan_object(json, b, 0, key, &vb, &ve) == detail::kNpos) return std::nullopt;
    if (vb == detail::kNpos) return std::nullopt;
    return json.substr(vb, ve - vb);
}

// Top-level string value. nullopt if absent or not a string (e.g. null).
inline std::optional<std::string> extract_top_level_string(const std::string& json,
                                                           std::string_view key) {
    auto raw = extract_top_level_raw(json, key);
    if (!raw || raw->empty() || (*raw)[0] != '"') return std::nullopt;
    return detail::decode_string(*raw, 0);
}

// Top-level object value, braces included — feed it back into these functions
// to read that object's own depth-0 keys. nullopt if absent or not an object.
inline std::optional<std::string> extract_top_level_object(const std::string& json,
                                                           std::string_view key) {
    auto raw = extract_top_level_raw(json, key);
    if (!raw || raw->empty() || (*raw)[0] != '{') return std::nullopt;
    return raw;
}

// Top-level integer. Accepts quoted digits, mirroring extract_int().
inline int64_t extract_top_level_int(const std::string& json, std::string_view key,
                                     int64_t fallback = 0) {
    auto raw = extract_top_level_raw(json, key);
    if (!raw || raw->empty()) return fallback;
    const char* p = raw->c_str();
    if (*p == '"') ++p;
    char* endp = nullptr;
    int64_t v = static_cast<int64_t>(std::strtoll(p, &endp, 10));
    if (endp == p) return fallback;
    return v;
}

// Top-level number. Accepts quoted digits, mirroring extract_number().
inline double extract_top_level_number(const std::string& json, std::string_view key,
                                       double fallback = 0.0) {
    auto raw = extract_top_level_raw(json, key);
    if (!raw || raw->empty()) return fallback;
    const char* p = raw->c_str();
    if (*p == '"') ++p;
    char* endp = nullptr;
    double v = std::strtod(p, &endp);
    if (endp == p) return fallback;
    return v;
}

// Top-level boolean. Anything that is not the literal true/false → fallback.
inline bool extract_top_level_bool(const std::string& json, std::string_view key,
                                   bool fallback = false) {
    auto raw = extract_top_level_raw(json, key);
    if (!raw) return fallback;
    if (*raw == "true") return true;
    if (*raw == "false") return false;
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
