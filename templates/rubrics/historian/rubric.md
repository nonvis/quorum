---
name: historian
version: v1
---

# Rubric: historian (v1)

Source: Quorum Specialties note "03 - Historian Quality Bar" (second-brain
vault). The historian is a thinker (analyst-class, read-only) that knows a
project's DECISION HISTORY — what was decided, when, why, and what got
pivoted/superseded — mined from git commit history, pull requests (open +
merged-to-main), and the curated Decision Log. It builds on a deterministic
Tier-1 record (`.quorum/historian/decisions-raw.json`) and persists a
structured, queryable `ref-decisions.md`. It reads the librarian's Decision Log
but never writes it.

The historian's core output is a factual claim, and the ground truth is git
history + the PRs + the Decision Log. The evaluator (analyst, Read/Grep +
read-only git) verifies the record against the sources: is each recorded
decision real and traceable to its cited PR#/commit/log entry (no fabrication),
is the status correct, are the supersession links right, is coverage complete.
That makes the first three categories (60%) objective; the pivot-reasoning tail
is the harder-weighted judgment dimension.

The `evaluator` agent (Phase 8 Track 1) reads this file, walks each item, and
emits per-item pass/fail in the EVALUATION block (Phase 8 Track 3). Categories
are documentation; per-item `(W)` weights drive scoring.

## Coverage / completeness (weight 20)
- [ ] (8) The major decisions across the Tier-1 record are captured — nothing major missed (verify against the recent commits + open PRs + merged-to-main PRs + Decision Log in `decisions-raw.json`)
- [ ] (7) Every merged-to-`main` PR that encodes a real choice is captured as a decision — merged-to-main PRs are the anchor signal, not optional
- [ ] (5) Routine churn (formatting, version bumps, lockfile updates) is NOT inflated into decisions — the "is this a decision" bar is applied

## Provenance accuracy (weight 20)
- [ ] (9) Every recorded decision cites a real source — a PR #, commit hash, or Decision Log entry that exists in the Tier-1 record. No fabricated decisions, no invented PR numbers or commit hashes
- [ ] (7) The cited source is the RIGHT one for the decision (a merged PR is cited by its PR #, not misattributed to an unrelated commit)
- [ ] (4) The decider/author is named and matches the source's `author` field (commit author or PR author) — not guessed

## Status & supersession tracking (weight 20)
- [ ] (8) Each decision is marked active / superseded / pivoted, and the mark is correct against the record (a decision reversed by a later PR/commit is NOT left "active")
- [ ] (8) Supersession chains are correct — the later PR/commit that replaces an earlier decision is linked to it, with direction right (X superseded by Y, not the reverse)
- [ ] (4) At least one clear pivot/reversal present in the record is caught — a merged decision later reverted/replaced is not missed

## Record form — structured & queryable (weight 10)
- [ ] (5) The decision history is a structured table (Decision · Date · Status · Source · Decider · Why), not free prose, so a decision lookup is answerable by reading the record
- [ ] (3) A "mined through <date>" staleness stamp is present so drift is visible
- [ ] (2) Frontmatter tags are present and the record is emitted as a single VAULT_UPDATE (no write into any repo)

## Pivot reasoning (weight 15)
- [ ] (8) For each pivoted/superseded decision, the record correctly explains WHAT changed (the earlier choice vs. the replacement)
- [ ] (4) The TRIGGER for the pivot is identified (what prompted the reversal — a later PR's rationale, a bug, a strategy change)
- [ ] (3) The reasoning is grounded in the record, not invented — a "why did we pivot" answer traces to a real PR body / commit message / log entry

## Query responsiveness (weight 10)
- [ ] (6) A direct decision-history question ("what did we decide about X?", "why did we pivot from Y to Z?") is answered correctly straight from the record, with the source cited
- [ ] (4) In-flight decisions are surfaced — notable open PRs are listed as decisions-in-progress (who, what, since when), distinct from the settled decisions

## Clarity + timeline renderability (weight 5)
- [ ] (3) Entries are concise + sourced — a reader can act on each decision row without follow-up
- [ ] (2) A requested decision-timeline / supersession description is complete + unambiguous enough for claude.ai to render (ordered decisions + the supersession edges)
