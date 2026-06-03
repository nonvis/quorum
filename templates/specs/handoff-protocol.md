# Quorum handoff protocol spec

> Contract for the markdown context files Quorum scribes write to `.quorum/` in the user's project workspace. Skill authors read this before authoring any skill that produces or consumes phase context. v0.1 scope: scribe-only.

Spec version: 0.2
Last updated: 2026-05-29
Source lineage: Suiperpower core/skills/data/specs/phase-handoff.md v1.0

## Why this spec exists

Skills do not pass state through global memory or a database. They pass it through plain markdown files on the user's filesystem. The user can read, edit, and version-control these files. The AI agent can read them across sessions. New skills can plug into the same files without coordination.

This spec defines:

- The context files and their canonical sections
- The append-only and non-deletion rules every writing skill follows
- The bootstrap rules that let any skill create a missing context file without forcing the user through a sequence
- The merge rules for re-runs

## File locations

```
<project-root>/
  .quorum/
    learnings.md              Written by scribe across sessions
```

v0.1 scope: scribe-only. Future expansion (doer / leader handoff schemas) is TBD and will extend this spec without breaking v0.1.

The folder is gitignored by default. Teams that want to commit context for collaboration can opt in by removing the rule.

## The five rules

### Rule 1: Canonical section headers

Every skill that writes to a context file uses these exact headers (no rewording, no synonym substitution). Other skills detect existence and append by header match.

`learnings.md` headers:

```
## Learnings, <UTC timestamp>     (per-session entry preceded by this heading)
  ### What we tried
  ### What worked
  ### What did not work
  ### Open questions
  ### Decisions
```

Written by: `scribe`.

### Rule 2: Append-only

Every list-shaped field is append-only across re-runs. Skills must:

1. Read the current file.
2. Locate the section by exact header match.
3. Append new bullets or rows.
4. Never delete or rewrite prior bullets unless the user explicitly asks.

### Rule 3: Non-deletion

A skill must not delete a section written by another skill. Example: `validate-idea` writes the **Validation** section. `competitive-landscape` writing the **Landscape** section must leave Validation intact.

If a skill detects a section that contradicts what it is about to write (e.g. a stale Business Model written before the user pivoted), it appends a new dated entry under the same section and notes the conflict, rather than overwriting.

Note: in v0.1 the scribe is the sole writer to `.quorum/learnings.md`, so non-deletion holds trivially. The rule is stated here so future writers (doer / leader handoff schemas) inherit the constraint without renegotiation.

### Rule 4: Bootstrap

Any skill can create a context file if it does not exist yet. The user may invoke skills in any order; they do not need to follow Learn → Idea → Build → Ship sequence.

When a skill needs a context file that does not exist:

1. Proceed immediately. Ask the user directly for the information needed for the section the skill is about to write.
2. Create the file. Write the canonical headers for sections the skill knows about, fill in only what was asked, leave optional sections out.
3. Do NOT redirect the user to run other commands first.
4. Do NOT print dependency chains or warn about missing files.

> The bootstrap rule is the difference between Suiperpower feeling like a flexible toolkit and a brittle pipeline. Skills cooperate; they do not gate each other.

### Rule 5: UTC timestamps + atomic writeback

Every section has a `Updated at: YYYY-MM-DDTHH:MM:SSZ` line near the top of the section. Skills update this when they write or modify the section. The bootstrap-creating skill writes `Created at:` at the top of the file with the same UTC ISO 8601 format.

Use UTC, not local time.

Atomic writeback: write the new content to `.quorum/learnings.md.tmp.<pid>` in the same directory, `fsync` the temp file, then `std::filesystem::rename` it over the canonical path. No torn writes; readers always see either the prior complete file or the new complete file.

## Write Mechanism (v0.2)

`.quorum/learnings.md` is populated by the daemon, not by scribe directly.
v0.1 designed scribe to use Edit/Write tools, but Sub-gate F (2026-05-29)
showed scribe is analyst-class at runtime and lacks Edit/Write access.
v0.2 ships a `LEARNINGS_UPDATE` block type the scribe emits in its output;
the daemon parses it and calls `apply_scribe_learnings_update()` to append
to the file.

Block format:

    LEARNINGS_UPDATE
    utc: <UTC ISO-8601>
    tried: |
      - bullet
    worked: |
      - bullet with evidence
    did_not_work: |
      - bullet with evidence
    open_questions: |
      - bullet as a question
    decisions: |
      - bullet with rationale

Each sub-field is optional. Empty sub-fields omit the entire `<field>: |`
line; the daemon does not render empty sub-sections.

The daemon enforces all five rules at the block boundary:
1. Canonical headers - sub-field names are fixed (no free-form headings).
2. Append-only - the primitive appends; never overwrites prior entries.
3. Non-deletion - trivially holds (scribe is the sole writer in v0.2).
4. Bootstrap - daemon creates `.quorum/learnings.md` on first block.
5. UTC + atomic - daemon uses tmp + fsync + rename, refreshes `Updated at:`.

The `--no-vault-write` conversation flag (Phase 10 Track 5) suppresses
`LEARNINGS_UPDATE` writes alongside `VAULT_UPDATE`, with a mirrored
`[dispatch] task N — LEARNINGS_UPDATE suppressed (...)` diagnostic.

## Canonical schema for .quorum/learnings.md

Bootstrap structure (created on first scribe write if file does not exist):

```
# Quorum project learnings

Created at: <UTC ISO-8601>
Updated at: <UTC ISO-8601>

> Append-only log. Every session entry below preceded by `## Learnings, <UTC>` heading.
```

Per-session entry (appended on each scribe write):

```
## Learnings, 2026-05-28T14:32:11Z

### What we tried
- <bullet, action verb first>

### What worked
- <bullet, with evidence>

### What did not work
- <bullet, with evidence and conclusion>

### Open questions
- <bullet, written as a question>

### Decisions
- <bullet, with rationale>
```

Field rules:

- All sub-section bullet lists are append-only forever.
- Empty sub-sections may be omitted (writer's choice, not gate-failing).
- `Updated at:` at file top is overwritten with each write's UTC timestamp.
- `Created at:` written once on bootstrap; never overwritten.

## Brainstorm gate invariant (leader handoff convention, Phase 14 T2)

The first concrete *leader* handoff convention (the "leader handoff schema"
flagged TBD above). In `brainstorm` mode, knowledge writes are **human-gated**:

1. The leader runs a **read-only** discussion (knowers + thinker contribute
   analysis only; **no `VAULT_UPDATE` is emitted during discussion**).
2. The leader emits findings + a per-knower proposed-vault-update manifest and
   `HANDOFF to: human` (→ `waiting_for_human`).
3. Only on `respond "yes[/edits]"` does the leader hand each approved knower a
   **write-now** instruction; the knower then emits its `VAULT_UPDATE` (its own
   vault, synthesized from the discussion).
4. `respond "no/more"` continues the discussion; the gate repeats.

**Invariant (L3): no knower emits a `VAULT_UPDATE` before the post-approval
write instruction.** A knower keys its write off the *task instruction* (a
direct-emit/write-now goal, e.g. the `run-knower.sh` single-knower scan, or the
leader's post-approval instruction) — **not** the mode. This keeps the
automated single-knower flow (no human in the loop) self-writing while the
interactive leader-driven brainstorm stays gated. Encoded in the leader and
knower SKILLs; the daemon already supplies `HANDOFF to: human` →
`waiting_for_human` + `respond` resume as primitives.

## Migration policy

When this spec changes:

1. Update this file first. Bump the spec version in the header.
2. Update every skill that writes to the affected file.
3. Update the testing strategy doc if a new validation test is needed.
4. Add a changelog entry below noting the breaking change (if any).

Skills are forward-compatible by default: they ignore unknown sections.

## Changelog

- **0.1** (2026-05-28): Initial spec. scribe-only scope. learnings.md with `## What we tried / What worked / What did not work / Open questions / Decisions` canonical sections. Append-only, non-deletion, bootstrap, UTC + atomic. Lifted from Suiperpower phase-handoff spec v1.0.
- **0.2** (2026-05-29): Reversed v0.1 Q12 lock ("tool-call-driven via SKILL.md text"). Sub-gate F (2026-05-29) empirically showed scribe is analyst-class at runtime and cannot Edit/Write directly - three production-scribe attempts against fresh fixtures all failed to write `.quorum/learnings.md`. v0.2 ships `LEARNINGS_UPDATE` block parsing in `output_parser.h` + daemon wiring in `main.cpp` task_dispatch that calls the existing `apply_scribe_learnings_update()` primitive. SKILL.md Job 4 rewritten to emit the block instead of (mythical) direct file writes. Five rules enforced at the block boundary rather than at the scribe-prompt boundary.
