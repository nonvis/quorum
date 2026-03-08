#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace sui::quorum {

// ─── Data structures ──────────────────────────────────────────────────────────

struct VaultUpdate {
    std::string path;     // e.g. "knowledge/analysis.md"
    std::string content;  // multi-line content to write
};

struct Proposal {
    std::string title;
    std::vector<std::string> requires_consensus_from;
    std::string content;
};

struct Review {
    std::string proposal_id;
    std::string verdict;   // "approve" | "reject" | "escalate"
    std::string reasoning;
};

struct ParsedObservation {
    std::string title;
    std::string agent;        // filled by main.cpp, not parsed from block
    std::string task_type;    // filled by main.cpp, not parsed from block
    std::vector<std::string> tags;
    std::string content;
};

struct ParsedOutput {
    std::string summary;                     // contents of SUMMARY block (empty if absent)
    std::vector<VaultUpdate> vault_updates;  // VAULT_UPDATE blocks
    std::vector<Proposal>    proposals;      // PROPOSAL blocks
    std::vector<Review>      reviews;        // REVIEW blocks
    std::vector<ParsedObservation> observations;  // OBSERVATION blocks
    std::string free_text;                   // everything outside structured blocks
    std::string raw;                         // original unmodified output
};

// ─── Parser ───────────────────────────────────────────────────────────────────

// Parses structured blocks from agent output.
//
// Recognized block types: VAULT_UPDATE, PROPOSAL, REVIEW, OBSERVATION, SUMMARY
//
// Three accepted formats (in order of precedence):
//
//   Format 1 — Explicit fence (original):
//     ```VAULT_UPDATE
//     path: knowledge/foo.md
//     content: |
//       multi-line content here
//     ```
//
//   Format 2 — Heading/bold label above a plain or language-tagged fence:
//     ## VAULT_UPDATE                          (or ### / # / **VAULT_UPDATE**)
//     ```                                     (plain fence)
//     path: knowledge/foo.md
//     content: |
//       multi-line content here
//     ```
//     Also accepted: ```markdown, ```sql, etc. when preceded by a type header.
//     A language-tagged fence without a preceding type header is ignored.
//     Heading suffixes are tolerated: "## VAULT_UPDATE — knowledge/foo.md"
//     Bold suffixes are tolerated:   "**VAULT_UPDATE: knowledge/foo.md**"
//
//   Format 3 — Type echoed as first line inside a plain fence:
//     ```
//     VAULT_UPDATE
//     path: knowledge/foo.md
//     content: |
//       multi-line content here
//     ```
//     If the type was already set by a heading above, a duplicate first line
//     is silently stripped (dedup). A plain ``` block with no type from any
//     source is silently ignored.
//
// Content fallback: if no explicit "content: |" field is found in a block,
// all block lines are joined and used as the content. This ensures blocks
// with free-text content (e.g. after a "---" separator) are not empty.
//
// VAULT_UPDATE path aliases: inside a VAULT_UPDATE block, the fields
// `target:` and `file:` are accepted as aliases for `path:`. The first
// non-empty value wins (checked in order: path → target → file).
//
// Bold metadata above fence: if bold metadata lines appear between a type
// header and the code fence (e.g. "**file:** `knowledge/foo.md`"), the
// path is captured and injected as a synthetic `path:` line at the start
// of the block. An explicit `path:`/`target:`/`file:` inside the block
// takes precedence (last-wins in parse_kv).
//
// Everything outside these blocks is collected into free_text.

class OutputParser {
public:
    [[nodiscard]] ParsedOutput parse(const std::string& raw_output) const {
        ParsedOutput result;
        result.raw = raw_output;

        auto lines = split_lines(raw_output);

        bool in_block = false;
        std::string block_type;
        std::vector<std::string> block_lines;
        std::string pending_type;
        std::string pending_path;

        for (const auto& line : lines) {
            if (!in_block) {
                auto btype = detect_block_open(line);
                if (!btype.empty()) {
                    in_block = true;
                    block_type = btype;
                    block_lines.clear();
                    pending_type.clear();
                    pending_path.clear();
                } else if (is_plain_fence(line)) {
                    in_block = true;
                    block_type = pending_type; // may be empty (tentative)
                    block_lines.clear();
                    if (!pending_path.empty() && block_type == "VAULT_UPDATE") {
                        block_lines.push_back("path: " + pending_path);
                    }
                } else if (!pending_type.empty() && starts_with_fence(line)) {
                    // Language-tagged fence (```markdown, ```sql, etc.)
                    // Only triggers when a type header was already seen above.
                    in_block = true;
                    block_type = pending_type;
                    block_lines.clear();
                    if (!pending_path.empty() && block_type == "VAULT_UPDATE") {
                        block_lines.push_back("path: " + pending_path);
                    }
                } else {
                    auto ht = detect_type_header(line);
                    if (!ht.empty()) {
                        pending_type = ht;
                        pending_path.clear();  // Reset path on new type header
                    }
                    // Extract path from bold metadata when expecting a VAULT_UPDATE
                    if (pending_type == "VAULT_UPDATE") {
                        auto mp = extract_metadata_path(line);
                        if (!mp.empty()) pending_path = mp;
                    }
                    if (!result.free_text.empty()) result.free_text += '\n';
                    result.free_text += line;
                }
            } else {
                if (is_block_close(line)) {
                    if (!block_type.empty()) {
                        dispatch_block(block_type, block_lines, result);
                    }
                    in_block = false;
                    block_type.clear();
                    block_lines.clear();
                    pending_type.clear();
                    pending_path.clear();
                } else {
                    // First-line type detection (Enhancement B)
                    if (block_lines.empty()) {
                        auto ft = trim(line);
                        bool skip = false;
                        for (const char* t : {"VAULT_UPDATE", "PROPOSAL",
                             "OBSERVATION", "SUMMARY", "REVIEW"}) {
                            if (ft == t) {
                                if (block_type.empty()) {
                                    block_type = t;
                                }
                                skip = (block_type == std::string(t));
                                break;
                            }
                        }
                        if (!skip) block_lines.push_back(line);
                    } else {
                        block_lines.push_back(line);
                    }
                }
            }
        }

        // Lenient: process an unclosed block rather than silently dropping it
        if (in_block && !block_type.empty() && !block_lines.empty()) {
            dispatch_block(block_type, block_lines, result);
        }

        // Trim trailing whitespace/newlines from free_text
        while (!result.free_text.empty() &&
               (result.free_text.back() == '\n' || result.free_text.back() == '\r' ||
                result.free_text.back() == ' ')) {
            result.free_text.pop_back();
        }

        return result;
    }

    // Returns true if the output contains at least one actionable item
    // (vault update, proposal, or review) that the daemon should act on.
    [[nodiscard]] bool has_actionable_output(const ParsedOutput& p) const {
        return !p.vault_updates.empty() || !p.proposals.empty() ||
               !p.reviews.empty() || !p.observations.empty();
    }

private:
    // ── Verdict normalization ──────────────────────────────────────────────────

    static std::string normalize_verdict(const std::string& raw) {
        // Lowercase
        std::string v;
        v.reserve(raw.size());
        for (auto c : raw) {
            v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        // Trim whitespace
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        while (!v.empty() && v.back() == ' ') v.pop_back();
        // Normalize aliases
        if (v == "approve" || v == "approved" || v == "accept" || v == "accepted") return "approve";
        if (v == "reject" || v == "rejected" || v == "deny" || v == "denied") return "reject";
        if (v == "revise" || v == "revision" || v == "revision_needed" || v == "needs_revision") return "revise";
        if (v == "escalate" || v == "escalated" || v == "needs_human" || v == "uncertain") return "escalate";
        return "reject";  // unknown -> reject (safe default)
    }

    // ── String helpers ─────────────────────────────────────────────────────────

    // Trim leading and trailing ASCII whitespace from a string_view.
    static std::string_view trim_sv(std::string_view sv) {
        while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r'))
            sv.remove_prefix(1);
        while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r'))
            sv.remove_suffix(1);
        return sv;
    }

    static std::string trim(std::string_view sv) {
        return std::string(trim_sv(sv));
    }

    // Split on '\n', stripping '\r' (handles both LF and CRLF).
    static std::vector<std::string> split_lines(const std::string& s) {
        std::vector<std::string> lines;
        std::string cur;
        for (char c : s) {
            if (c == '\r') continue;
            if (c == '\n') {
                lines.push_back(std::move(cur));
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) lines.push_back(std::move(cur));
        return lines;
    }

    // Join a vector of lines into a single string separated by newlines.
    // Trailing newlines are stripped.
    static std::string join_lines(const std::vector<std::string>& lines) {
        std::string result;
        for (const auto& l : lines) {
            result += l;
            result += '\n';
        }
        while (!result.empty() && result.back() == '\n')
            result.pop_back();
        return result;
    }

    // Parse "[item1, item2, item3]" → vector of trimmed strings.
    static std::vector<std::string> parse_list(std::string_view sv) {
        sv = trim_sv(sv);
        if (!sv.empty() && sv.front() == '[') sv.remove_prefix(1);
        if (!sv.empty() && sv.back()  == ']') sv.remove_suffix(1);

        std::vector<std::string> items;
        size_t start = 0;
        for (size_t i = 0; i <= sv.size(); ++i) {
            if (i == sv.size() || sv[i] == ',') {
                auto item = trim(sv.substr(start, i - start));
                if (!item.empty()) items.push_back(std::move(item));
                start = i + 1;
            }
        }
        return items;
    }

    // ── Block detection ────────────────────────────────────────────────────────

    // Returns the block type label (e.g. "VAULT_UPDATE") if the line opens a
    // known structured block, otherwise returns an empty string.
    static std::string detect_block_open(std::string_view line) {
        // Strip leading whitespace
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        if (line.size() < 3 || line.substr(0, 3) != "```") return {};
        line.remove_prefix(3);
        auto type = trim(line);
        if (type == "VAULT_UPDATE" || type == "PROPOSAL" ||
            type == "REVIEW"       || type == "SUMMARY"  || type == "OBSERVATION") {
            return type;
        }
        return {};
    }

    // Returns true for a closing fence: optional whitespace + "```" + only whitespace.
    static bool is_block_close(std::string_view line) {
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        if (line.size() < 3 || line.substr(0, 3) != "```") return false;
        line.remove_prefix(3);
        // Only trailing whitespace is allowed after the backticks
        line = trim_sv(line);
        return line.empty();
    }

    // Returns true if line is a plain code fence (``` with only whitespace after).
    // Identical logic to is_block_close() — both recognize the same pattern.
    static bool is_plain_fence(std::string_view line) {
        return is_block_close(line);
    }

    // Returns true if line starts with ``` (possibly followed by a language tag).
    // Unlike is_plain_fence(), this matches ```markdown, ```sql, etc.
    static bool starts_with_fence(std::string_view line) {
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        return line.size() >= 3 && line.substr(0, 3) == "```";
    }

    // Detects a known block type from a markdown heading or bold label line.
    // Handles:  ## VAULT_UPDATE   ### OBSERVATION   # REVIEW
    //           **PROPOSAL**      **VAULT_UPDATE: path/here.md**
    // Returns the type string (e.g. "VAULT_UPDATE") or empty string.
    static std::string detect_type_header(std::string_view line) {
        // Strip leading whitespace
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);

        // Strip heading prefix (#, ##, ###, etc.)
        if (!line.empty() && line.front() == '#') {
            while (!line.empty() && line.front() == '#')
                line.remove_prefix(1);
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                line.remove_prefix(1);
        }

        // Strip bold markers (**)
        if (line.size() >= 2 && line.substr(0, 2) == "**")
            line.remove_prefix(2);
        if (line.size() >= 2 && line.substr(line.size() - 2, 2) == "**")
            line.remove_suffix(2);

        // Strip whitespace again after marker removal
        line = trim_sv(line);

        // Check if remainder starts with a known type followed by non-alpha or end
        for (const char* t : {"VAULT_UPDATE", "PROPOSAL",
             "OBSERVATION", "SUMMARY", "REVIEW"}) {
            std::string_view type_sv(t);
            if (line.size() >= type_sv.size() &&
                line.substr(0, type_sv.size()) == type_sv) {
                if (line.size() == type_sv.size()) return std::string(type_sv);
                char next = line[type_sv.size()];
                // Non-alpha: any char that is not A-Z or a-z
                bool is_alpha = (next >= 'A' && next <= 'Z') ||
                                (next >= 'a' && next <= 'z');
                if (!is_alpha) return std::string(type_sv);
            }
        }
        return {};
    }

    // Extract a vault path from bold metadata lines between a type header
    // and a code fence. Recognizes patterns like:
    //   **target:** knowledge/foo.md
    //   **file:** `knowledge/foo.md`
    //   **path:** knowledge/foo.md
    // Returns the extracted path or empty string.
    static std::string extract_metadata_path(std::string_view line) {
        line = trim_sv(line);
        // Must start with **
        if (line.size() < 2 || line.substr(0, 2) != "**") return {};
        line.remove_prefix(2);

        // Check for known path labels
        for (const char* label : {"target:", "file:", "path:"}) {
            std::string_view lbl(label);
            if (line.size() >= lbl.size() &&
                line.substr(0, lbl.size()) == lbl) {
                line.remove_prefix(lbl.size());
                // Strip closing ** if present
                if (line.size() >= 2 && line.substr(0, 2) == "**")
                    line.remove_prefix(2);
                // Trim whitespace, then strip backticks
                auto path = trim_sv(line);
                if (!path.empty() && path.front() == '`') path.remove_prefix(1);
                if (!path.empty() && path.back() == '`') path.remove_suffix(1);
                // Strip trailing ** that might appear after backtick-wrapped paths
                auto result = trim_sv(path);
                if (result.size() >= 2 && result.substr(result.size() - 2) == "**")
                    result.remove_suffix(2);
                result = trim_sv(result);
                return result.empty() ? std::string{} : std::string(result);
            }
        }
        return {};
    }

    // ── Key-value parsing ──────────────────────────────────────────────────────

    // Lightweight key-value bag for parsing block fields.
    struct KVBag {
        struct Val {
            std::string              text;
            std::vector<std::string> list;
            bool                     is_list{false};
        };
        // Ordered insertion; lookup from back to get last-wins semantics.
        std::vector<std::pair<std::string, Val>> fields;

        std::string get_str(std::string_view key) const {
            for (auto it = fields.rbegin(); it != fields.rend(); ++it)
                if (it->first == key) return it->second.text;
            return {};
        }

        std::vector<std::string> get_list(std::string_view key) const {
            for (auto it = fields.rbegin(); it != fields.rend(); ++it) {
                if (it->first == key) {
                    if (it->second.is_list) return it->second.list;
                    if (!it->second.text.empty()) return {it->second.text};
                    return {};
                }
            }
            return {};
        }
    };

    // Parse block lines as a key-value map.
    //
    // Supported field forms:
    //   key: value              → simple string
    //   key: [item1, item2]     → list
    //   key: |                  → multi-line (subsequent lines with 2-space indent)
    static KVBag parse_kv(const std::vector<std::string>& lines) {
        KVBag bag;
        std::string cur_key;
        bool        in_ml = false;
        std::string ml_content;

        // Flush the current multi-line field into the bag.
        auto flush = [&]() {
            if (!cur_key.empty() && in_ml) {
                while (!ml_content.empty() && ml_content.back() == '\n')
                    ml_content.pop_back();
                KVBag::Val v;
                v.text = std::move(ml_content);
                bag.fields.emplace_back(cur_key, std::move(v));
                cur_key.clear();
            }
            in_ml = false;
            ml_content.clear();
        };

        // Parse one line as a key: value entry.
        auto process_kv_line = [&](const std::string& line) {
            auto colon = line.find(':');
            if (colon == std::string::npos) return;

            std::string_view lsv = line;
            auto key = trim(lsv.substr(0, colon));
            if (key.empty()) return;

            auto val = trim(lsv.substr(colon + 1));

            if (val == "|") {
                flush();
                cur_key = key;
                in_ml   = true;
                ml_content.clear();
            } else if (!val.empty() && val.front() == '[') {
                flush();
                KVBag::Val v;
                v.is_list = true;
                v.list    = parse_list(val);
                bag.fields.emplace_back(key, std::move(v));
            } else {
                flush();
                KVBag::Val v;
                v.text = std::string(val);
                bag.fields.emplace_back(key, std::move(v));
            }
        };

        for (const auto& line : lines) {
            if (in_ml) {
                if (line.size() >= 2 && line[0] == ' ' && line[1] == ' ') {
                    // Strip the 2-space indent prefix
                    ml_content += line.substr(2);
                    ml_content += '\n';
                } else if (line.empty()) {
                    // Blank line: preserve as empty line within multi-line content
                    ml_content += '\n';
                } else {
                    // Non-indented, non-empty line ends the multi-line field
                    flush();
                    process_kv_line(line);
                }
            } else {
                process_kv_line(line);
            }
        }
        flush();
        return bag;
    }

    // ── Block dispatch ─────────────────────────────────────────────────────────

    static void dispatch_block(const std::string&              type,
                               const std::vector<std::string>& lines,
                               ParsedOutput&                   out) {
        if (type == "SUMMARY") {
            std::string s;
            for (size_t i = 0; i < lines.size(); ++i) {
                if (i) s += '\n';
                s += lines[i];
            }
            // Trim trailing whitespace
            while (!s.empty() &&
                   (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                s.pop_back();
            out.summary = std::move(s);

        } else if (type == "VAULT_UPDATE") {
            auto bag = parse_kv(lines);
            VaultUpdate vu;
            vu.path    = bag.get_str("path");
            if (vu.path.empty()) vu.path = bag.get_str("target");
            if (vu.path.empty()) vu.path = bag.get_str("file");
            vu.content = bag.get_str("content");
            if (vu.content.empty() && !lines.empty()) {
                vu.content = join_lines(lines);
            }
            out.vault_updates.push_back(std::move(vu));

        } else if (type == "PROPOSAL") {
            auto bag = parse_kv(lines);
            Proposal p;
            p.title                   = bag.get_str("title");
            p.requires_consensus_from = bag.get_list("requires_consensus_from");
            if (p.requires_consensus_from.empty())
                p.requires_consensus_from = bag.get_list("required_reviewers");
            if (p.requires_consensus_from.empty())
                p.requires_consensus_from = bag.get_list("reviewers");
            p.content                 = bag.get_str("content");
            if (p.content.empty() && !lines.empty()) {
                p.content = join_lines(lines);
            }
            out.proposals.push_back(std::move(p));

        } else if (type == "REVIEW") {
            auto bag = parse_kv(lines);
            Review r;
            r.proposal_id = bag.get_str("proposal_id");
            if (r.proposal_id.empty()) r.proposal_id = bag.get_str("proposal");
            auto raw_verdict = bag.get_str("verdict");
            if (raw_verdict.empty()) raw_verdict = bag.get_str("decision");
            r.verdict = raw_verdict.empty() ? "reject" : normalize_verdict(raw_verdict);
            r.reasoning   = bag.get_str("reasoning");
            out.reviews.push_back(std::move(r));

        } else if (type == "OBSERVATION") {
            auto bag = parse_kv(lines);
            ParsedObservation obs;
            obs.title   = bag.get_str("title");
            obs.tags    = bag.get_list("tags");
            obs.content = bag.get_str("content");
            if (obs.content.empty() && !lines.empty()) {
                obs.content = join_lines(lines);
            }
            // agent and task_type are filled by the daemon, not parsed
            out.observations.push_back(std::move(obs));
        }
    }
};

} // namespace sui::quorum
