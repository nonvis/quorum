#pragma once

// Phase 8 Track 2 — Rubric format + loader + resolver.
//
// Rubrics are weighted markdown checklists shipped at:
//   templates/rubrics/<role-specialty>/rubric.md      (with the daemon)
//   .quorum/rubrics/<role-specialty>/rubric.md         (per-project override)
//
// Format (codified by the parser below):
//   - YAML frontmatter with `name` and `version` (REQUIRED — missing → nullopt).
//   - H1 line is descriptive only (ignored by the parser).
//   - H2 sections are categories. Optional `(weight N)` annotation on the H2
//     line is retained but not currently used for scoring (per-item weights
//     drive total_weight).
//   - Items: `- [ ] (N) <description>` where N is a POSITIVE int per-item
//     weight. The checkbox state is ignored — the markdown defines the rubric,
//     not the score.
//   - Item IDs are auto-derived: <category-slug>.<description-slug>, where
//     slugify lowercases, replaces non-alphanumeric runs with hyphens, trims.
//   - Items without a `(N)` weight are skipped silently (test C: clean policy
//     so authors can keep TODO bullets in-file without breaking the parser).
//   - Items with weight ≤ 0 are skipped with a stderr warning.
//   - Duplicate item IDs keep the first occurrence; subsequent duplicates
//     warn to stderr.
//
// The evaluator (Phase 8 Track 1) consumes the parsed Rubric to score work.
// The EVALUATION block format (Track 3) and the move-dev rubric content
// (Track 4) are separate cycles.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace sui::quorum {

// One scored line in a rubric. id is auto-derived from category + description.
struct RubricItem {
    std::string id;          // "<category-slug>.<description-slug>"
    std::string description; // raw item text after the `(N)` weight
    std::string category;    // H2 section title (raw)
    int weight = 0;          // positive int — items with weight ≤ 0 are dropped
};

// A parsed rubric ready for the evaluator. total_weight is the sum of
// item weights (max raw score). Score normalization to 0..100 is the
// evaluator's job — the parser only exposes raw structure.
struct Rubric {
    std::string name;        // frontmatter `name`
    std::string version;     // frontmatter `version`
    std::vector<RubricItem> items;
    int total_weight = 0;
    std::string source_path; // for debugging / log lines
};

namespace rubric_detail {

// trim ASCII whitespace from both ends.
[[nodiscard]] inline std::string trim(std::string_view sv) {
    size_t b = 0;
    size_t e = sv.size();
    while (b < e && (sv[b] == ' ' || sv[b] == '\t' || sv[b] == '\r')) ++b;
    while (e > b && (sv[e - 1] == ' ' || sv[e - 1] == '\t' || sv[e - 1] == '\r')) --e;
    return std::string(sv.substr(b, e - b));
}

// Slugify per parser contract: lowercase, replace runs of non-alphanumeric
// with a single '-', strip leading/trailing '-'.
[[nodiscard]] inline std::string slugify(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool in_dash = true; // suppress leading dash
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            out.push_back(static_cast<char>(std::tolower(uc)));
            in_dash = false;
        } else if (!in_dash) {
            out.push_back('-');
            in_dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

// Parse `- [ ] (N) description` (allowing leading whitespace and any checkbox
// state inside the brackets). Returns false if the line doesn't match the
// item shape OR if the `(N)` weight is missing/non-numeric. The latter is
// intentionally lumped in with "not an item line" so the parser can skip
// silently — see the format docs above.
[[nodiscard]] inline bool parse_item_line(const std::string& raw,
                                          int& weight_out,
                                          std::string& desc_out) {
    // Skip leading whitespace.
    size_t i = 0;
    while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t')) ++i;
    // Require '-' followed by space.
    if (i + 1 >= raw.size()) return false;
    if (raw[i] != '-' || raw[i + 1] != ' ') return false;
    i += 2;
    // Require '[<any-single-char>]' (typical: '[ ]', '[x]', '[X]').
    if (i + 2 >= raw.size()) return false;
    if (raw[i] != '[' || raw[i + 2] != ']') return false;
    i += 3;
    // Require one space after the checkbox.
    if (i >= raw.size() || raw[i] != ' ') return false;
    ++i;
    // Require '(N)' immediately. If we don't find it, this isn't a scored
    // item — skip the line.
    if (i >= raw.size() || raw[i] != '(') return false;
    size_t close = raw.find(')', i + 1);
    if (close == std::string::npos) return false;
    auto weight_str = trim(raw.substr(i + 1, close - i - 1));
    if (weight_str.empty()) return false;
    // Parse integer (allow leading '-' for the validation path that warns
    // on ≤ 0). std::stoi throws on non-numeric → treated as "not an item".
    int w = 0;
    try {
        size_t consumed = 0;
        w = std::stoi(weight_str, &consumed);
        if (consumed != weight_str.size()) return false;
    } catch (...) {
        return false;
    }
    weight_out = w;
    // Description = remainder of line, trimmed.
    size_t desc_start = close + 1;
    if (desc_start < raw.size() && raw[desc_start] == ' ') ++desc_start;
    desc_out = trim(raw.substr(desc_start));
    if (desc_out.empty()) return false;
    return true;
}

// Read entire file to string. Returns empty string on failure (caller treats
// "no content" as parse failure).
[[nodiscard]] inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

// Strip the optional `(weight N)` annotation off an H2 title and return the
// raw category name. The annotation is currently retained as documentation
// only — per-item weights drive total_weight.
[[nodiscard]] inline std::string strip_category_weight(const std::string& title) {
    auto open = title.rfind('(');
    if (open == std::string::npos) return trim(title);
    auto close = title.find(')', open + 1);
    if (close == std::string::npos) return trim(title);
    auto inner = trim(title.substr(open + 1, close - open - 1));
    if (inner.rfind("weight ", 0) == 0) {
        return trim(title.substr(0, open));
    }
    return trim(title);
}

}  // namespace rubric_detail

// Loads a rubric from `path`. Returns nullopt on parse error or missing file.
// Errors are logged to stderr — never thrown.
[[nodiscard]] inline std::optional<Rubric> load_rubric(
    const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    if (!fs::exists(path) || !fs::is_regular_file(path)) {
        // Quiet on missing file — resolve_rubric() expects this to be a
        // common case (template exists, override doesn't).
        return std::nullopt;
    }

    auto content = rubric_detail::read_file(path);
    if (content.empty()) {
        std::cerr << "WARNING: rubric file empty or unreadable: "
                  << path.string() << "\n";
        return std::nullopt;
    }

    // Split into lines. Tolerate CRLF (rubric_detail::trim drops trailing \r).
    std::vector<std::string> lines;
    {
        std::string line;
        std::istringstream iss(content);
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
        }
    }

    // Frontmatter must be the FIRST non-empty line and start with '---'.
    // This is intentionally strict so a rubric file without frontmatter
    // fails fast (test B).
    size_t cursor = 0;
    while (cursor < lines.size() && rubric_detail::trim(lines[cursor]).empty()) {
        ++cursor;
    }
    if (cursor >= lines.size() || rubric_detail::trim(lines[cursor]) != "---") {
        std::cerr << "WARNING: rubric missing YAML frontmatter: "
                  << path.string() << "\n";
        return std::nullopt;
    }
    ++cursor; // step past opening '---'

    Rubric rubric;
    rubric.source_path = path.string();

    // Walk frontmatter until closing '---'. Recognize `name:` and `version:`.
    bool fm_closed = false;
    while (cursor < lines.size()) {
        auto trimmed = rubric_detail::trim(lines[cursor]);
        ++cursor;
        if (trimmed == "---") { fm_closed = true; break; }
        if (trimmed.empty()) continue;
        auto colon = trimmed.find(':');
        if (colon == std::string::npos) continue;
        auto key = rubric_detail::trim(trimmed.substr(0, colon));
        auto val = rubric_detail::trim(trimmed.substr(colon + 1));
        // Strip optional surrounding quotes.
        if (val.size() >= 2 &&
            ((val.front() == '"' && val.back() == '"') ||
             (val.front() == '\'' && val.back() == '\''))) {
            val = val.substr(1, val.size() - 2);
        }
        if (key == "name") rubric.name = val;
        else if (key == "version") rubric.version = val;
    }
    if (!fm_closed) {
        std::cerr << "WARNING: rubric frontmatter unterminated: "
                  << path.string() << "\n";
        return std::nullopt;
    }
    if (rubric.name.empty() || rubric.version.empty()) {
        std::cerr << "WARNING: rubric frontmatter missing 'name' or 'version': "
                  << path.string() << "\n";
        return std::nullopt;
    }

    // Walk body. Track current category; append items under it.
    std::string current_category;
    std::string current_category_slug;
    std::unordered_set<std::string> seen_ids;

    for (; cursor < lines.size(); ++cursor) {
        const auto& raw = lines[cursor];
        auto trimmed = rubric_detail::trim(raw);
        if (trimmed.empty()) continue;

        // Defensive: skip '#' comment lines OUTSIDE any category and OUTSIDE
        // any markdown heading (markdown headers also start with '#'). We
        // treat '# ' (h1), '## ' (h2), '### ' (h3) etc. as headings; only
        // a bare '#' followed by anything other than '#' or space is a
        // comment in the rubric grammar. In practice this branch is rarely
        // taken — markdown rubrics don't use '#' as a comment.
        if (!trimmed.empty() && trimmed[0] == '#') {
            // Determine heading level (count leading '#').
            size_t h = 0;
            while (h < trimmed.size() && trimmed[h] == '#') ++h;
            // Must be followed by a space to be a real heading.
            if (h < trimmed.size() && trimmed[h] == ' ') {
                if (h == 1) {
                    // H1 — descriptive title only, ignored.
                    continue;
                }
                if (h == 2) {
                    // H2 — new category.
                    auto title = rubric_detail::trim(trimmed.substr(h + 1));
                    current_category = rubric_detail::strip_category_weight(title);
                    current_category_slug = rubric_detail::slugify(current_category);
                    continue;
                }
                // H3+ — treat as informational, skip.
                continue;
            }
            // Bare '#...' (no space): treated as comment, skip.
            continue;
        }

        // Frontmatter delimiter inside body? Treat as ignorable.
        if (trimmed == "---") continue;

        // Try parsing as an item line. Note: parse_item_line takes the RAW
        // line (with leading whitespace) since the format allows leading
        // whitespace before the dash.
        int weight = 0;
        std::string desc;
        if (!rubric_detail::parse_item_line(raw, weight, desc)) {
            // Not a recognized item — skip silently. Authors can keep prose
            // bullets / TODO notes in the file without breaking the parser.
            continue;
        }
        if (current_category.empty()) {
            std::cerr << "WARNING: rubric item before any H2 category — skipped: "
                      << path.string() << " :: " << desc << "\n";
            continue;
        }
        if (weight <= 0) {
            std::cerr << "WARNING: rubric item weight ≤ 0 — skipped: "
                      << path.string() << " :: " << desc << " (weight="
                      << weight << ")\n";
            continue;
        }

        RubricItem item;
        item.category = current_category;
        item.description = desc;
        item.weight = weight;
        auto desc_slug = rubric_detail::slugify(desc);
        item.id = current_category_slug + "." + desc_slug;

        if (!seen_ids.insert(item.id).second) {
            std::cerr << "WARNING: duplicate rubric item id '" << item.id
                      << "' — keeping first, skipping: "
                      << path.string() << "\n";
            continue;
        }

        rubric.total_weight += item.weight;
        rubric.items.push_back(std::move(item));
    }

    return rubric;
}

// Resolve a rubric by role-specialty:
//   1. <project_root>/.quorum/rubrics/<role-specialty>/rubric.md  (override)
//   2. <cwd>/templates/rubrics/<role-specialty>/rubric.md         (template)
//
// The template lookup mirrors the existing `templates/<...>` resolution used
// by agent_create.h (`role_template`): CWD-relative, with a small ladder of
// fallback candidates so unit tests that run from build/ still find the
// shipped templates. The fallback ladder is:
//      templates/rubrics/<rs>/rubric.md
//      ../templates/rubrics/<rs>/rubric.md
//      ../../templates/rubrics/<rs>/rubric.md
//
// If neither override nor template exists, returns nullopt — the evaluator
// is expected to handle "no rubric available" gracefully (Track 3).
[[nodiscard]] inline std::optional<Rubric> resolve_rubric(
    const std::string& project_root,
    const std::string& role_specialty) {
    namespace fs = std::filesystem;

    if (role_specialty.empty()) {
        return std::nullopt;
    }

    // 1. Project-level override always wins.
    if (!project_root.empty()) {
        auto override_path = fs::path(project_root) / ".quorum" / "rubrics" /
            role_specialty / "rubric.md";
        if (fs::exists(override_path) && fs::is_regular_file(override_path)) {
            return load_rubric(override_path);
        }
    }

    // 2. Shipped template fallback (CWD-relative, with small ladder).
    const std::vector<fs::path> candidates = {
        fs::path("templates") / "rubrics" / role_specialty / "rubric.md",
        fs::path("..") / "templates" / "rubrics" / role_specialty / "rubric.md",
        fs::path("..") / ".." / "templates" / "rubrics" / role_specialty / "rubric.md",
    };
    for (const auto& c : candidates) {
        if (fs::exists(c) && fs::is_regular_file(c)) {
            return load_rubric(c);
        }
    }

    return std::nullopt;
}

}  // namespace sui::quorum
