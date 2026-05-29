---
name: historian-pivot
description: Produce a decision history from a Tier-1 record that contains a clear pivot (merged decision reverted by an open PR). Offline — no live gh.
---

# Historian Pivot Benchmark

## Goal

A deterministic Tier-1 mine has already run and left its record at
`decisions-raw.json` in the workspace root (for this benchmark the record is
pre-baked there; in a normal project it lives at
`.quorum/historian/decisions-raw.json`). **Read `decisions-raw.json` from the
workspace root.** It covers two repos (`gateway`, `contracts`) — recent commits
(with authors), open PRs, and PRs merged to `main`. The workspace also has a
curated `00 - Decision Log.md`.

Produce the decision history per the historian bar, working **entirely from
the Tier-1 record + the Decision Log** (do NOT run `gh` or hit the network —
the mining is already done). Specifically:

1. Build the structured decision history: for each significant decision, its
   date, status (active / superseded / pivoted), source (PR # / commit / log
   entry), decider, and the "why."
2. Catch the pivot. There is a clear supersession chain in `gateway`: the
   session store was decided, then changed, then is being reverted. Identify
   the chain and the trigger.
3. Surface in-flight decisions: list the notable open PR(s) as
   decisions-in-progress.

## Constraints

- Work from `decisions-raw.json` (workspace root) and `00 - Decision Log.md`.
  Do NOT re-mine (no `gh`, no network).
- Merged-to-`main` PRs are first-class decisions — capture them.
- Do NOT inflate routine churn (a dependency bump, a prettier run) into
  decisions.
- Every decision must cite a REAL source from the record — a PR #, a commit
  hash, or a Decision Log entry. Do not fabricate PR numbers or commit hashes.
- Do NOT write the project Decision Log — read it only.
- The record must be a STRUCTURED table, with a "mined through" staleness stamp.

## What to deliver

- A structured decision-history record (table) covering the significant
  decisions across both repos, with status + provenance + decider + why.
- The supersession chain for the session store, with its trigger.
- The open PR(s) listed as in-flight.

The evaluator scores this against the historian rubric. The Tier-1 record +
the Decision Log are the ground truth; coverage, provenance, and
status/supersession tracking are weighted here.
