# Architect synthetic benchmark suite

Synthetic tasks for the `architect` knower — a thinker (analyst, read-only)
that maps a project's STRUCTURE (components + how they interconnect) and reasons
about it (primary flow, change-impact). Each task ships a tiny multi-module
fixture in `expected/` with real cross-module imports/calls; the daemon runs the
standard `leader → architect → evaluator → scribe` pipeline and the evaluator
scores the produced map against the architect rubric
(`templates/rubrics/architect/rubric.md`).

## Layout

```
templates/benchmarks/architect/<task-name>/
  task.md            REQUIRED. Frontmatter + Goal + Constraints + Deliverables.
  expected/          The fixture CODEBASE (a handful of modules with real
                     cross-module imports/calls). Copied into the temp project
                     before agents start. Ground truth for the dependency graph
                     — NOT an output to diff.
  evaluator-notes.md The ground-truth component inventory + edge list (the true
                     edges) + flow/blast-radius answers the evaluator checks
                     accuracy + no-hallucination against.
```

The code's real dependency graph is the ground truth (contrast move-dev, where
ground truth is `sui move build` / tests). The evaluator verifies every claimed
edge against the actual imports/call sites.

## Run

```bash
quorum benchmark --role architect --task architect-map
quorum benchmark --role architect            # all tasks
```

## Tasks

| Task              | Difficulty | Hits |
|-------------------|------------|------|
| architect-map     | easy       | component inventory + interconnection accuracy + primary-flow trace |
| architect-impact  | medium     | change-impact / blast-radius reasoning along real edges |

`architect-map` is a strict layered chain (api→service→repo→db + a leaf models
module) — the no-hallucination check has obvious wrong answers (layer-skip
edges). `architect-impact` tests blast-radius reasoning: two direct callers, one
transitive, one insulated component.
