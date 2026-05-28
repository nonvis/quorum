# Quorum handoff protocol spec

> Contract for the markdown context files Quorum scribes write to `.quorum/` in the user's project workspace. Skill authors read this before authoring any skill that produces or consumes phase context. v0.1 scope: scribe-only.

Spec version: 0.1
Last updated: 2026-05-28
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

v0.1 scope: scribe-only. Future expansion (doer / reviewer / leader handoff schemas) is TBD and will extend this spec without breaking v0.1.

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

Note: in v0.1 the scribe is the sole writer to `.quorum/learnings.md`, so non-deletion holds trivially. The rule is stated here so future writers (doer / reviewer / leader handoff schemas) inherit the constraint without renegotiation.

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

## Migration policy

When this spec changes:

1. Update this file first. Bump the spec version in the header.
2. Update every skill that writes to the affected file.
3. Update the testing strategy doc if a new validation test is needed.
4. Add a changelog entry below noting the breaking change (if any).

Skills are forward-compatible by default: they ignore unknown sections.

## Changelog

- **0.1** (2026-05-28): Initial spec. scribe-only scope. learnings.md with `## What we tried / What worked / What did not work / Open questions / Decisions` canonical sections. Append-only, non-deletion, bootstrap, UTC + atomic. Lifted from Suiperpower phase-handoff spec v1.0.
