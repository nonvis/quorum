# historian-pivot — evaluator notes

The Tier-1 record (`decisions-raw.json` at the workspace root, copied from
`expected/`) plus `00 - Decision Log.md` ARE the ground truth. (In a normal
project the record lives at `.quorum/historian/decisions-raw.json`; this
benchmark pre-bakes it at the root so it survives `quorum init`.) Score the
produced decision history against them — there is no live `gh` and no network;
everything the historian needs is in the record. This benchmark stresses
**coverage**, **provenance accuracy**, and **status & supersession tracking**
(the pivot), which together carry 60% of the bar.

## Ground-truth decisions (what the record MUST capture)

| Decision | Date | Status | Source | Decider | Why |
|----------|------|--------|--------|---------|-----|
| Session store on Postgres | 2026-05-12 | superseded → then re-adopted (in-flight) | PR #118 (gateway) / commit a1b2c3d / log | Priya Nadar | durable session store, replaces in-memory map |
| Sessions moved to Redis | 2026-05-21 | pivoted / being reverted | PR #131 (gateway) | Dao Le | lower read latency |
| Sliding-log rate limiting | 2026-05-20 | active | PR #124 (gateway) / commit c7d8e9f | Tomas Berg | replace fixed-window rate limiting |
| Registry UpgradeCap = 2-of-3 multisig | 2026-05-10 | active | PR #57 (contracts) / commit f1e2d3c / log | Mara Quinn | secure upgrade authority |

Four significant decisions. The two `gateway` commits `e4f5a6b` (axios bump)
and `0a1b2c3` (prettier run), and the `contracts` commit `b4a5c6d` (test
coverage), are routine churn — they must NOT be inflated into decisions.

## The pivot the historian MUST catch (supersession chain)

The session-store decision has a 3-step chain in `gateway`:

1. **PR #118** (2026-05-12, Priya Nadar) — adopt **Postgres** session store.
2. **PR #131** (2026-05-21, Dao Le) — **superseded** #118: move sessions to
   **Redis** for lower read latency.
3. **Open PR #142** (since 2026-05-28, Priya Nadar) — **reverts** #131: go back
   to Postgres-only. This is IN-FLIGHT (open, not merged).

A correct answer states: Postgres (#118) → Redis (#131, superseded #118) →
revert to Postgres (open #142, in-flight). The **trigger** for the revert is in
PR #142's title — going back to Postgres-only (the Redis move is being undone).
Marking #131 as still "active," or missing that #142 is only OPEN (not a
settled decision), is wrong.

## Provenance the historian MUST cite (no fabrication)

Real sources only — these exist in the record:
- PRs: `#118`, `#131`, `#124` (gateway), `#57` (contracts), open `#142` (gateway).
- Commits: `a1b2c3d`, `c7d8e9f` (gateway), `f1e2d3c` (contracts).
- Deciders: Priya Nadar, Dao Le, Tomas Berg (gateway); Mara Quinn (contracts) —
  must match the `author` field of the cited source.

Any invented PR number / commit hash / decider fails Provenance accuracy.

## In-flight (open PRs)

Exactly one open PR: `gateway #142` "Revert Redis session store; go back to
Postgres-only" — Priya Nadar, since 2026-05-28. It must be listed as in-flight,
NOT as a settled/merged decision.

## Boundary the historian MUST respect

`00 - Decision Log.md` is read as a source (it confirms #118 and #57) but must
NOT be written/appended by the historian. The Redis pivot (#131) and the
sliding-log rate-limit decision (#124) are MISSING from the curated log — a
strong historian flags them as candidates for the operator but does not append
to the log itself.

## Scoring emphasis

- **Coverage / completeness (20)** — all four real decisions captured; the
  three churn commits NOT inflated.
- **Provenance accuracy (20)** — every decision cites a real PR#/commit/log
  entry with the right decider; nothing fabricated.
- **Status & supersession tracking (20)** — the Postgres → Redis → revert chain
  is correct, direction right, and #142 is in-flight not settled.
- **Pivot reasoning (15)** — explains what changed (Postgres→Redis→Postgres)
  and the trigger (the revert).

## Items that matter LESS

- Clarity + timeline renderability (5) — small dock if the timeline/supersession
  description is thin, as long as the chain itself is right.
