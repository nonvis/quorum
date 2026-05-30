#pragma once

// Phase 13 Track 4 — `quorum scribe record` CLI (autopilot output parity).
//
// The autopilot supervisor runs a scribe (analyst-class) and captures the
// ```LEARNINGS_UPDATE``` block it emits. This CLI pipes that block through the
// EXACT same parse + write path the daemon uses, so an autopilot run accumulates
// .quorum/learnings.md BYTE-IDENTICALLY to a daemon run. Parity comes from
// REUSE, not reimplementation:
//
//   OutputParser().parse(raw)              -> ParsedOutput.learnings_updates
//   apply_scribe_learnings_update(root, e) -> .quorum/learnings.md (atomic)
//
// This is the same call sequence main.cpp's task-dispatch loop runs over each
// ScribeLearningsEntry. There is NO new write code here and NO `claude -p` call
// anywhere in this file — the supervisor (or the operator) provides the block
// text on stdin or via a file; this CLI only parses + applies.
//
// ARCHITECTURE (mirrors cli/librarian_curate.h): the testable core is a PURE
// function (apply_learnings_blocks) that takes the raw block text as a string
// and touches no process I/O beyond the shared write primitive. run_scribe_record
// is the thin CLI shell (resolve root, slurp input, print summary). The PURE
// core is what test_autopilot_parity.cpp drives to prove Sub-gate D.
//
// Header-only, matches the cli/librarian_curate.h convention.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "agent/output_parser.h"   // OutputParser, ParsedOutput
#include "vault/scribe_writer.h"   // ScribeLearningsEntry, apply_scribe_learnings_update

namespace sui::quorum::cli {

struct ScribeRecordOptions {
    std::string project_path;   // resolved target project root (parent of .quorum/); empty = cwd
    std::string block_file;     // path to the LEARNINGS_UPDATE block text; empty = read stdin
};

struct ScribeRecordResult {
    int applied = 0;                        // entries written ok
    int failed = 0;                         // entries that failed to write
    bool bootstrapped = false;              // true if any apply bootstrapped a fresh file
    std::vector<std::string> diagnostics;   // human-readable reasons (rejections / "none found")
};

// PURE core. Parse the raw text for LEARNINGS_UPDATE blocks and apply each one
// to project_root via the SAME daemon primitive. No process I/O beyond the
// shared atomic write inside apply_scribe_learnings_update — this is exactly the
// daemon's task-dispatch loop over parsed.learnings_updates.
//
// This is the parity seam: an autopilot run that calls this and a daemon run
// that loops apply_scribe_learnings_update over the same ParsedOutput produce
// byte-identical .quorum/learnings.md (the timestamp is carried by the block's
// utc: field, so no wall-clock divergence).
[[nodiscard]] inline ScribeRecordResult apply_learnings_blocks(
    const std::string& project_root, const std::string& raw_block_text) {
    ScribeRecordResult result;

    OutputParser parser;
    auto parsed = parser.parse(raw_block_text);

    if (parsed.learnings_updates.empty()) {
        result.diagnostics.push_back("no LEARNINGS_UPDATE block found");
        return result;
    }

    for (const auto& entry : parsed.learnings_updates) {
        auto r = sui::quorum::apply_scribe_learnings_update(project_root, entry);
        if (r.ok) {
            ++result.applied;
            if (r.bootstrapped) result.bootstrapped = true;
        } else {
            ++result.failed;
            result.diagnostics.push_back(r.reason);
        }
    }

    return result;
}

namespace detail {

// Slurp an entire stream into a string. Used for both stdin and a file handle.
[[nodiscard]] inline std::string slurp_stream(std::istream& in) {
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace detail

// Top-level entrypoint for `quorum scribe record`. Orchestrates:
//   1. resolve project_path (default cwd)
//   2. read the LEARNINGS_UPDATE block text (from --block-file, else stdin)
//   3. apply_learnings_blocks (the parity seam)
//   4. print a summary; diagnostics to stderr
//
// Return 0 iff at least one entry applied AND nothing failed; 1 otherwise
// (nothing applied, or any failure). NO `claude -p` call.
[[nodiscard]] inline int run_scribe_record(const ScribeRecordOptions& opts) {
    namespace fs = std::filesystem;

    // 1. Resolve project root.
    std::string project_root = opts.project_path;
    if (project_root.empty()) {
        project_root = fs::current_path().string();
    }

    // 2. Read the block text.
    std::string raw;
    if (!opts.block_file.empty()) {
        std::ifstream f(opts.block_file, std::ios::binary);
        if (!f.is_open()) {
            std::cerr << "ERROR: cannot open block file: " << opts.block_file
                      << "\n";
            return 1;
        }
        raw = detail::slurp_stream(f);
    } else {
        raw = detail::slurp_stream(std::cin);
    }

    // 3. Apply (the parity seam).
    auto result = apply_learnings_blocks(project_root, raw);

    // 4. Summary.
    std::cout << result.applied << " applied, " << result.failed << " failed";
    if (result.bootstrapped) {
        std::cout << " (bootstrapped .quorum/learnings.md)";
    }
    std::cout << "\n";

    for (const auto& d : result.diagnostics) {
        std::cerr << "  " << d << "\n";
    }

    return (result.applied > 0 && result.failed == 0) ? 0 : 1;
}

}  // namespace sui::quorum::cli
