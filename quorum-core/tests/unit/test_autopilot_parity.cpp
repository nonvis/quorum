// Phase 13 Track 4 — autopilot OUTPUT PARITY test (manual-acceptance Sub-gate D).
//
// Proves an autopilot scribe run accumulates .quorum/learnings.md BYTE-IDENTICALLY
// to the daemon, because both drive the SAME parse + write path:
//
//   daemon:    OutputParser p; auto parsed = p.parse(block);
//              for (auto& e : parsed.learnings_updates)
//                  apply_scribe_learnings_update(root, e);
//
//   autopilot: apply_learnings_blocks(root, block)   // cli/scribe_record.h
//              -- which internally runs EXACTLY the loop above.
//
// The block carries its UTC timestamp in the `utc:` field, so the rendered
// "Created at:" / "Updated at:" lines are deterministic (no wall-clock), which
// is what makes byte-identical equality (not "differs only in timestamps")
// the correct assertion.
//
// Plain int main() + asserts, temp dirs under /tmp, no test framework. Mirrors
// tests/unit/test_scribe_write_discipline.cpp.

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

#include "agent/output_parser.h"   // OutputParser, ParsedOutput
#include "vault/scribe_writer.h"   // apply_scribe_learnings_update, detail::read_file_text
#include "cli/scribe_record.h"     // apply_learnings_blocks (the autopilot path)

namespace fs = std::filesystem;
using sui::quorum::OutputParser;
using sui::quorum::apply_scribe_learnings_update;
using sui::quorum::detail::read_file_text;
using sui::quorum::cli::apply_learnings_blocks;

// One valid LEARNINGS_UPDATE block, exactly the shape the scribe emits
// (templates/skills/quorum-roles/scribe/SKILL.md / handoff-protocol.md):
// utc: + multi-line `key: |` sub-fields whose lines are 2-space-indented "- "
// bullets. Empty sub-sections are simply omitted by the scribe.
static const char* kBlock1 =
    "```LEARNINGS_UPDATE\n"
    "utc: 2026-05-30T09:10:00Z\n"
    "tried: |\n"
    "  - wired apply_learnings_blocks to the daemon primitive\n"
    "  - piped a captured LEARNINGS_UPDATE block to the CLI\n"
    "worked: |\n"
    "  - byte-identical accumulation across daemon and autopilot engines\n"
    "decisions: |\n"
    "  - parity comes from REUSE of apply_scribe_learnings_update, not a fork\n"
    "```\n";

// A second block with a DIFFERENT utc:, used for the multi-block input case.
static const char* kBlock2 =
    "```LEARNINGS_UPDATE\n"
    "utc: 2026-05-30T11:45:30Z\n"
    "tried: |\n"
    "  - applied two blocks in a single autopilot call\n"
    "did_not_work: |\n"
    "  - relying on wall-clock timestamps (would break parity)\n"
    "open_questions: |\n"
    "  - does multi-block input preserve append order across engines\n"
    "```\n";

// DAEMON PATH: replicate main.cpp's task-dispatch loop verbatim over a root.
// Returns the parsed entry count so callers can cross-check the autopilot count.
static size_t daemon_apply(const std::string& root, const std::string& block) {
    OutputParser p;
    auto parsed = p.parse(block);
    for (const auto& e : parsed.learnings_updates) {
        auto r = apply_scribe_learnings_update(root, e);
        assert(r.ok && "daemon path: apply_scribe_learnings_update failed");
    }
    return parsed.learnings_updates.size();
}

static void cleanup(const std::string& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

static void expect_equal(const std::string& daemon_content,
                         const std::string& autopilot_content,
                         const char* label) {
    if (daemon_content != autopilot_content) {
        std::cerr << "PARITY FAILURE (" << label << "):\n"
                  << "----- daemon -----\n" << daemon_content << "\n"
                  << "----- autopilot -----\n" << autopilot_content << "\n"
                  << "----- end -----\n";
        assert(false && "daemon and autopilot content diverged");
    }
}

int main() {
    const std::string pid = std::to_string(::getpid());
    const std::string rootA = "/tmp/quorum_parity_daemon_" + pid;
    const std::string rootB = "/tmp/quorum_parity_autopilot_" + pid;
    const std::string rootC = "/tmp/quorum_parity_daemon_multi_" + pid;
    const std::string rootD = "/tmp/quorum_parity_autopilot_multi_" + pid;

    // Start from clean slates.
    cleanup(rootA);
    cleanup(rootB);
    cleanup(rootC);
    cleanup(rootD);

    const std::string block(kBlock1);

    // ── Assertion 1: first write (bootstrap) parity ──────────────────────────
    {
        size_t daemon_count = daemon_apply(rootA, block);

        auto autopilot = apply_learnings_blocks(rootB, block);
        assert(static_cast<size_t>(autopilot.applied) == daemon_count &&
               "autopilot applied count must equal parsed entry count");
        assert(autopilot.failed == 0 && "autopilot must have zero failures");
        assert(daemon_count == 1 && "fixture block must parse to one entry");

        auto daemon_content =
            read_file_text(fs::path(rootA) / ".quorum" / "learnings.md");
        auto autopilot_content =
            read_file_text(fs::path(rootB) / ".quorum" / "learnings.md");
        assert(!daemon_content.empty() && "daemon learnings.md must be non-empty");
        expect_equal(daemon_content, autopilot_content, "bootstrap");
    }
    std::cout << "[1/3] bootstrap parity: byte-identical OK\n";

    // ── Assertion 2: append parity (apply the SAME block a second time) ───────
    {
        size_t daemon_count = daemon_apply(rootA, block);

        auto autopilot = apply_learnings_blocks(rootB, block);
        assert(static_cast<size_t>(autopilot.applied) == daemon_count &&
               "append: autopilot applied count must equal parsed entry count");
        assert(autopilot.failed == 0 && "append: autopilot must have zero failures");

        auto daemon_content =
            read_file_text(fs::path(rootA) / ".quorum" / "learnings.md");
        auto autopilot_content =
            read_file_text(fs::path(rootB) / ".quorum" / "learnings.md");
        expect_equal(daemon_content, autopilot_content, "append");
    }
    std::cout << "[2/3] append parity (Updated-at refresh + append-only): "
                 "byte-identical OK\n";

    // ── Assertion 3: multi-block input parity ────────────────────────────────
    // One string containing TWO LEARNINGS_UPDATE blocks with different utc:.
    // Daemon loops both entries; autopilot makes one call. Both must match.
    {
        const std::string multi = std::string(kBlock1) + "\n" + kBlock2;

        size_t daemon_count = daemon_apply(rootC, multi);
        assert(daemon_count == 2 && "multi-block fixture must parse to two entries");

        auto autopilot = apply_learnings_blocks(rootD, multi);
        assert(static_cast<size_t>(autopilot.applied) == daemon_count &&
               "multi: autopilot applied count must equal parsed entry count");
        assert(autopilot.failed == 0 && "multi: autopilot must have zero failures");

        auto daemon_content =
            read_file_text(fs::path(rootC) / ".quorum" / "learnings.md");
        auto autopilot_content =
            read_file_text(fs::path(rootD) / ".quorum" / "learnings.md");
        assert(!daemon_content.empty() && "multi: daemon learnings.md non-empty");
        expect_equal(daemon_content, autopilot_content, "multi-block");
    }
    std::cout << "[3/3] multi-block input parity: byte-identical OK\n";

    // Clean up temp dirs.
    cleanup(rootA);
    cleanup(rootB);
    cleanup(rootC);
    cleanup(rootD);

    std::cout << "All autopilot parity tests passed (Sub-gate D)\n";
    return 0;
}
