#pragma once

// Phase 10 Track 4 — `quorum vault audit` CLI helpers.
//
// Walks rule-*.md / ref-*.md files in a vault dir and surfaces those whose
// `last_reviewed` is older than --days (default 90) or whose `expiry` is in
// the past. Read-only — never edits or deletes files (plan #17). Files with
// neither field, or with both fields fresh, are silently skipped.
//
// Header-only, matches the cli/vault_dedup.h pattern.

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "agent/context_assembler.h"   // classify_knowledge_filename, KnowledgeKind
#include "utils/frontmatter.h"         // parse_frontmatter_field

namespace sui::quorum::cli {

struct VaultAuditOptions {
    std::string vault_path;     // resolved target dir; non-empty by run_vault_audit call
    int days = 90;              // last_reviewed staleness threshold; --days flag overrides
    bool use_global = false;    // user passed --global (informational; resolved before run)
};

namespace detail {

// Parse "YYYY-MM-DD" (optionally followed by more chars, e.g. "T10:00:00Z")
// into a UTC time_t at 00:00:00 on that date. Returns std::nullopt on any
// anomaly (sscanf miss, year/month/day out of range, mktime fail). Lax on
// Feb-30 — operators do not type Feb 30 in practice.
//
// timegm() is POSIX and exists on macOS + Linux glibc/musl; the codebase
// already targets BSD/Linux only (see vault_dedup.h convention).
[[nodiscard]] inline std::optional<std::time_t> parse_iso_date(
    const std::string& s) {
    int y = 0, m = 0, d = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) < 3) return std::nullopt;
    if (y < 1970 || y > 9999) return std::nullopt;
    if (m < 1 || m > 12) return std::nullopt;
    if (d < 1 || d > 31) return std::nullopt;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon  = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    tm.tm_isdst = 0;
    std::time_t t = timegm(&tm);
    if (t == static_cast<std::time_t>(-1)) return std::nullopt;
    return t;
}

// Whole days between two time_t values (later - earlier) / 86400, truncated.
// Negative if `later < earlier`.
[[nodiscard]] inline long days_between(std::time_t earlier, std::time_t later) {
    return static_cast<long>((later - earlier) / 86400);
}

struct AuditFinding {
    std::filesystem::path path;
    bool stale_last_reviewed = false;   // last_reviewed older than threshold
    bool expired = false;               // expiry in the past
    std::string last_reviewed_raw;      // verbatim from frontmatter (for display)
    std::string expiry_raw;
    long last_reviewed_age_days = 0;    // only set when stale_last_reviewed
    long expiry_past_days = 0;          // only set when expired
};

// Read a file fully into a std::string. Returns empty string on open failure.
// Local helper (distinct from vault_dedup.h's identically-named helper to
// avoid in-TU redefinition when main.cpp includes both headers).
[[nodiscard]] inline std::string audit_read_file_string(const std::filesystem::path& p) {
    std::ifstream in(p);
    if (!in.is_open()) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Walk dir non-recursively, classify each rule-*.md / ref-*.md file by
// frontmatter fields. Files with neither field, or with both fields fresh,
// are NOT returned. `now` is "today" at 00:00:00 UTC; `threshold_days` is
// opts.days. Returns findings sorted by filename for deterministic output.
[[nodiscard]] inline std::vector<AuditFinding> scan_vault(
    const std::filesystem::path& dir,
    std::time_t now,
    int threshold_days,
    size_t& total_scanned_out) {
    total_scanned_out = 0;
    std::vector<AuditFinding> findings;
    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
        return findings;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto fname = entry.path().filename().string();
        auto kind = sui::quorum::classify_knowledge_filename(fname);
        if (kind != sui::quorum::KnowledgeKind::Rule &&
            kind != sui::quorum::KnowledgeKind::Reference) continue;
        ++total_scanned_out;

        std::string content = audit_read_file_string(entry.path());
        std::string lr_raw = sui::quorum::parse_frontmatter_field(content, "last_reviewed");
        std::string ex_raw = sui::quorum::parse_frontmatter_field(content, "expiry");
        if (lr_raw.empty() && ex_raw.empty()) continue;

        AuditFinding f;
        f.path = entry.path();
        if (!lr_raw.empty()) {
            auto lr = parse_iso_date(lr_raw);
            if (lr.has_value()) {
                long age = days_between(*lr, now);
                if (age > threshold_days) {
                    f.stale_last_reviewed = true;
                    f.last_reviewed_age_days = age;
                    f.last_reviewed_raw = lr_raw;
                }
            }
        }
        if (!ex_raw.empty()) {
            auto ex = parse_iso_date(ex_raw);
            if (ex.has_value() && *ex < now) {
                f.expired = true;
                f.expiry_past_days = days_between(*ex, now);
                f.expiry_raw = ex_raw;
            }
        }
        if (f.stale_last_reviewed || f.expired) {
            findings.push_back(std::move(f));
        }
    }
    std::sort(findings.begin(), findings.end(),
        [](const AuditFinding& a, const AuditFinding& b) {
            return a.path.filename().string() < b.path.filename().string();
        });
    return findings;
}

} // namespace detail

// Top-level entrypoint. Caller (main.cpp) is responsible for resolving
// opts.vault_path before calling — walking --global / --vault / default-to-
// project-root logic and dropping a concrete directory in opts.vault_path.
//
// Return codes:
//   0  — success (incl. "no files" / "no flags", which are not errors)
//   1  — vault path missing / not a directory / empty (internal error)
//   2  — --global with empty global_knowledge_path (handled in main.cpp)
[[nodiscard]] inline int run_vault_audit(const VaultAuditOptions& opts) {
    namespace fs = std::filesystem;
    if (opts.vault_path.empty()) {
        std::cerr << "ERROR: vault audit: empty vault path (internal error)\n";
        return 1;
    }
    fs::path vault(opts.vault_path);
    if (!fs::exists(vault) || !fs::is_directory(vault)) {
        std::cerr << "ERROR: vault path does not exist or is not a directory: "
                  << opts.vault_path << "\n";
        return 1;
    }

    std::time_t now = std::time(nullptr);
    // Normalize 'now' to UTC midnight so day-granularity math is stable
    // regardless of when in the day the command runs.
    {
        std::tm utc = *std::gmtime(&now);
        utc.tm_hour = 0; utc.tm_min = 0; utc.tm_sec = 0;
        now = timegm(&utc);
    }

    size_t total = 0;
    auto findings = detail::scan_vault(vault, now, opts.days, total);

    if (total == 0) {
        std::cout << "No rule-*.md or ref-*.md files found in " << opts.vault_path << "\n";
        return 0;
    }

    // Partition findings into three groups (stale-only, expired-only, both),
    // preserving the filename-sorted order inside each group.
    std::vector<const detail::AuditFinding*> stale_only, expired_only, both;
    for (const auto& f : findings) {
        if (f.stale_last_reviewed && f.expired) both.push_back(&f);
        else if (f.stale_last_reviewed) stale_only.push_back(&f);
        else if (f.expired) expired_only.push_back(&f);
    }

    auto print_stale_line = [](std::ostream& os, const detail::AuditFinding& f) {
        os << "  " << f.path.filename().string()
           << "  (last_reviewed: " << f.last_reviewed_raw
           << ", " << f.last_reviewed_age_days << " days ago)\n";
    };
    auto print_expired_line = [](std::ostream& os, const detail::AuditFinding& f) {
        os << "  " << f.path.filename().string()
           << "  (expiry: " << f.expiry_raw
           << ", " << f.expiry_past_days << " days ago)\n";
    };
    auto print_both_line = [](std::ostream& os, const detail::AuditFinding& f) {
        os << "  " << f.path.filename().string()
           << "  (last_reviewed: " << f.last_reviewed_raw
           << ", " << f.last_reviewed_age_days << " days ago; expiry: "
           << f.expiry_raw << ", " << f.expiry_past_days << " days ago)\n";
    };

    if (!stale_only.empty()) {
        std::cout << "Stale (last_reviewed > " << opts.days << " days):\n";
        for (auto* f : stale_only) print_stale_line(std::cout, *f);
        std::cout << "\n";
    }
    if (!expired_only.empty()) {
        std::cout << "Expired (expiry past):\n";
        for (auto* f : expired_only) print_expired_line(std::cout, *f);
        std::cout << "\n";
    }
    if (!both.empty()) {
        std::cout << "Stale + Expired:\n";
        for (auto* f : both) print_both_line(std::cout, *f);
        std::cout << "\n";
    }

    size_t flagged = findings.size();
    std::cout << flagged << " file(s) flagged out of " << total << " scanned.\n";
    return 0;
}

} // namespace sui::quorum::cli
