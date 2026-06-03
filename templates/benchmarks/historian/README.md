# Historian synthetic benchmark suite

Synthetic tasks for the `historian` knower — a thinker (analyst, read-only)
that knows a project's DECISION HISTORY (what was decided, when, why, what got
pivoted) and serves decision-history questions. Each task ships a **hand-authored
Tier-1 record** (`expected/.quorum/historian/decisions-raw.json`) — the same
shape `scripts/historian_mine.py` produces — plus a curated Decision Log, so the
task tests **Tier-2 interpretation offline** (no `gh`, no network). The daemon
runs the standard `leader → historian → evaluator` pipeline and the
evaluator scores the produced `ref-decisions.md` against the historian rubric
(`templates/rubrics/historian/rubric.md`).

## Layout

```
templates/benchmarks/historian/<task-name>/
  task.md            REQUIRED. Frontmatter + Goal + Constraints + Deliverables.
  expected/          The fixture WORKSPACE. Copied into the temp project before
                     agents start. Holds:
                       decisions-raw.json    (the Tier-1 record — at the root so
                                              it survives `quorum init`; the
                                              task.md points the historian to it.
                                              In a normal project it lives at
                                              .quorum/historian/decisions-raw.json)
                       00 - Decision Log.md  (curated log source)
                     This is the ground truth the decision history is scored
                     against — NOT an output to diff.
  evaluator-notes.md The ground-truth decisions table + the pivot/supersession
                     chain + the provenance the historian must cite.
```

The record is open-form, so there is no rigid `expected/` output file. The
fixture's Tier-1 record + Decision Log are the ground truth; the evaluator walks
the produced history against them with Read / Grep.

## Run

```bash
quorum benchmark --role historian --task historian-pivot
quorum benchmark --role historian            # all tasks
```

## Tasks

| Task             | Difficulty | Hits |
|------------------|------------|------|
| historian-pivot  | medium     | coverage + provenance + status/supersession (a 3-step pivot chain) + in-flight open PR + curated-log boundary |

The pivot task is offline by construction (the Tier-1 mining is pre-baked into
the fixture), so it spends no `gh`/network — only the Tier-2 LLM interpretation.
