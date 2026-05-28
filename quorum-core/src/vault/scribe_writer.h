#pragma once

// Phase 10 Track 10 #52 - scribe write-discipline primitive (handoff protocol v0.1).
//
// This header implements the C++ side of the scribe write-discipline contract
// defined in templates/specs/handoff-protocol.md (v0.1). The production scribe
// writes .quorum/learnings.md through its own tool-call access (Edit/Write)
// per SKILL.md Job 4 - the daemon does NOT learn a new LEARNINGS_UPDATE block
// type in v0.1. This primitive exists as:
//
//   1. Test-bait. test_scribe_write_discipline.cpp drives it directly to
//      verify bootstrap, append-only, timestamp discipline, and canonical
//      header rejection - the four assertions from the parent plan #52(a-d).
//
//   2. Future enforcement seam. If discipline drift surfaces in Sub-gate F or
//      real use, v0.2 can wire a LEARNINGS_UPDATE block type into main.cpp's
//      task_dispatch loop and call apply_scribe_learnings_update from there.
//
// The primitive is intentionally free-standing - no daemon, no DB, no
// VaultManager coupling. .quorum/learnings.md lives at the PROJECT root, not
// under the per-agent vault tree at {base_dir}/vaults/<agent_id>/. The
// existing VaultManager::apply_vault_update path requires "knowledge/" or
// "inbox/" prefixes and is a different write surface (see Track 10
// Implementation Plan, Risks & Traps section 2).
//
// Header-only, matches the vault/context_history.h convention.

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace sui::quorum {

// One session's worth of learnings, structured per spec v0.1 canonical schema.
// Empty vectors mean "omit this sub-section" (per spec: empty sub-sections may
// be omitted; writer's choice, not gate-failing).
struct ScribeLearningsEntry {
    std::string utc_timestamp;            // "2026-05-28T14:32:11Z"
    std::vector<std::string> tried;       // bullets, may be empty
    std::vector<std::string> worked;
    std::vector<std::string> did_not_work;
    std::vector<std::string> open_questions;
    std::vector<std::string> decisions;
};

// Outcome of a scribe primitive call. On rejection, `reason` carries a
// human-readable diagnostic suitable for stderr logging.
struct ScribeLearningsResult {
    bool ok{false};
    std::string reason;
    bool bootstrapped{false};
};

namespace detail {

// The five canonical sub-section headings, in spec v0.1 order. Used by both
// the validator (#52d) and the writer (which only emits these).
inline const std::vector<std::string>& canonical_sub_headings() {
    static const std::vector<std::string> kCanonical{
        "What we tried",
        "What worked",
        "What did not work",
        "Open questions",
        "Decisions",
    };
    return kCanonical;
}

inline bool is_canonical_heading(std::string_view h) {
    const auto& canonical = canonical_sub_headings();
    return std::find(canonical.begin(), canonical.end(), h) != canonical.end();
}

inline std::string read_file_text(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Atomic write: write to .tmp.<pid>, fsync, rename. Returns true on success;
// populates `err` with a human-readable message on failure.
inline bool atomic_write_text(const std::filesystem::path& target,
                              const std::string& content,
                              std::string& err) {
    auto tmp = target;
    tmp += ".tmp." + std::to_string(::getpid());

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "scribe_writer: failed to open temp file: " + tmp.string();
            return false;
        }
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!f) {
            err = "scribe_writer: failed to write temp file: " + tmp.string();
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
        f.flush();
    }

    // fsync the temp file so the renamed payload is durable.
    int fd = ::open(tmp.c_str(), O_RDONLY);
    if (fd >= 0) {
        (void)::fsync(fd);
        ::close(fd);
    }

    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        err = "scribe_writer: failed to rename temp into place: " + ec.message();
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        return false;
    }
    return true;
}

// Render a single sub-section. Returns empty string if `bullets` is empty
// (caller omits the sub-section heading entirely in that case).
inline std::string render_subsection(std::string_view heading,
                                     const std::vector<std::string>& bullets) {
    if (bullets.empty()) return {};
    std::string out;
    out += "### ";
    out += heading;
    out += "\n";
    for (const auto& b : bullets) {
        out += "- ";
        out += b;
        out += "\n";
    }
    out += "\n";
    return out;
}

// Render one session entry (starts with "## Learnings, <ts>" heading).
inline std::string render_session_entry(const ScribeLearningsEntry& entry) {
    std::string out;
    out += "## Learnings, ";
    out += entry.utc_timestamp;
    out += "\n\n";
    out += render_subsection("What we tried", entry.tried);
    out += render_subsection("What worked", entry.worked);
    out += render_subsection("What did not work", entry.did_not_work);
    out += render_subsection("Open questions", entry.open_questions);
    out += render_subsection("Decisions", entry.decisions);
    return out;
}

// Bootstrap file content (created on first write to a fresh project). Matches
// spec v0.1 Canonical schema section verbatim.
inline std::string render_bootstrap_preamble(std::string_view utc_timestamp) {
    std::string out;
    out += "# Quorum project learnings\n\n";
    out += "Created at: ";
    out += utc_timestamp;
    out += "\n";
    out += "Updated at: ";
    out += utc_timestamp;
    out += "\n\n";
    out += "> Append-only log. Every session entry below preceded by ";
    out += "`## Learnings, <UTC>` heading.\n\n";
    return out;
}

// Replace the "Updated at: <ts>" line at the file top with the new timestamp.
// Leaves the "Created at:" line untouched (immutable per Rule 5 / spec
// Canonical schema field rules). Returns the rewritten content.
inline std::string refresh_updated_at(const std::string& existing,
                                      std::string_view new_timestamp) {
    const std::string marker = "Updated at: ";
    auto pos = existing.find(marker);
    if (pos == std::string::npos) {
        // File exists but has no Updated at: line - return unchanged. Caller
        // will append the new entry, but the timestamp discipline assertion
        // will fail visibly rather than silently.
        return existing;
    }
    auto eol = existing.find('\n', pos);
    if (eol == std::string::npos) eol = existing.size();
    std::string out;
    out.reserve(existing.size() + new_timestamp.size());
    out.append(existing, 0, pos);
    out += marker;
    out += new_timestamp;
    out.append(existing, eol, std::string::npos);
    return out;
}

}  // namespace detail

// Pure-function classifier: accepts a vector of sub-section headings and
// returns ok=true iff every heading is in the canonical set. Used by the
// scribe (or any future caller that parses raw markdown into structured
// entries) to reject non-canonical sub-headings BEFORE any disk write.
//
// Subset (omitting some canonical headings) is accepted - per spec, empty
// sub-sections may be omitted. Order is not enforced (writer's choice).
//
// Does NOT validate timestamp format - that is a separate concern; the
// production scribe is responsible for providing UTC ISO-8601 timestamps.
[[nodiscard]] inline ScribeLearningsResult validate_canonical_headers(
    const std::vector<std::string>& section_headings) {
    ScribeLearningsResult result;
    for (const auto& h : section_headings) {
        if (!detail::is_canonical_heading(h)) {
            result.ok = false;
            result.reason = "non-canonical sub-heading rejected: '" + h +
                            "' (allowed: What we tried, What worked, What did "
                            "not work, Open questions, Decisions)";
            return result;
        }
    }
    result.ok = true;
    return result;
}

// File primitive: read .quorum/learnings.md under project_root, append a
// session entry per spec v0.1, atomic write. Bootstraps the file with the
// canonical preamble if missing (Rule 4). Returns ok=true with
// bootstrapped=true on the first write to a fresh project.
//
// On re-append:
//   - Read existing content
//   - Update the "Updated at:" line at the top to entry.utc_timestamp
//   - Leave "Created at:" byte-identical (Rule 5 immutability)
//   - Append the new session block at end of file (Rule 2 append-only)
//   - Write atomically via .tmp.<pid> + fsync + rename (Rule 5 atomic)
//
// Does NOT validate that the entry's headings are canonical - validation is
// caller's responsibility via validate_canonical_headers. The structured
// ScribeLearningsEntry type makes non-canonical sub-section names impossible
// by construction (the five vectors are the only write surface).
[[nodiscard]] inline ScribeLearningsResult apply_scribe_learnings_update(
    std::string_view project_root,
    const ScribeLearningsEntry& entry) {
    ScribeLearningsResult result;

    if (entry.utc_timestamp.empty()) {
        result.ok = false;
        result.reason = "scribe_writer: entry.utc_timestamp is empty";
        return result;
    }

    std::filesystem::path root(project_root);
    auto quorum_dir = root / ".quorum";
    auto learnings_path = quorum_dir / "learnings.md";

    std::error_code ec;
    std::filesystem::create_directories(quorum_dir, ec);
    if (ec) {
        result.ok = false;
        result.reason = "scribe_writer: failed to create .quorum/ dir: " +
                        ec.message();
        return result;
    }

    bool exists = std::filesystem::exists(learnings_path, ec);
    std::string new_content;

    if (!exists) {
        // Bootstrap: preamble + first session entry.
        new_content = detail::render_bootstrap_preamble(entry.utc_timestamp);
        new_content += detail::render_session_entry(entry);
        result.bootstrapped = true;
    } else {
        // Append: refresh Updated at: + concatenate the new session block.
        auto existing = detail::read_file_text(learnings_path);
        auto refreshed = detail::refresh_updated_at(existing, entry.utc_timestamp);

        // Make sure the appended block is separated from prior content by a
        // single blank line (the rendered entry already ends each section
        // with one trailing newline; we want one blank line before the new
        // ## heading).
        if (!refreshed.empty() && refreshed.back() != '\n') {
            refreshed += '\n';
        }
        // Ensure exactly one blank line separator between prior content and
        // the new session heading.
        while (refreshed.size() >= 2 &&
               refreshed[refreshed.size() - 1] == '\n' &&
               refreshed[refreshed.size() - 2] == '\n') {
            refreshed.pop_back();
        }
        refreshed += "\n";  // restore single trailing newline
        refreshed += detail::render_session_entry(entry);

        new_content = std::move(refreshed);
        result.bootstrapped = false;
    }

    std::string err;
    if (!detail::atomic_write_text(learnings_path, new_content, err)) {
        result.ok = false;
        result.reason = err;
        return result;
    }

    result.ok = true;
    return result;
}

}  // namespace sui::quorum
