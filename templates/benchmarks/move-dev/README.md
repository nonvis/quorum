# Move-dev synthetic benchmark suite

Phase 8 Track 5: 5 self-contained Move tasks the daemon runs through the
standard `leader → move-dev → evaluator → scribe` team pipeline. Each
benchmark exercises a distinct slice of the move-dev rubric
(`templates/rubrics/move-dev/rubric.md`).

## Layout

```
templates/benchmarks/<role-specialty>/<task-name>/
  task.md            REQUIRED. Frontmatter (name + description) + Goal +
                     Constraints + Deliverables. Body becomes the
                     conversation goal seeded into `quorum converse`.
  expected/          OPTIONAL. Pre-populated starter files (e.g. buggy
                     module to fix, verbose module to refactor) copied
                     into the temp project before the agents start.
  evaluator-notes.md OPTIONAL. Task-specific scoring guidance the
                     evaluator may consult alongside the rubric.
```

`expected/` is a starter-file directory, NOT a fixture for output
comparison. The rubric drives correctness assessment; we don't diff
files.

## Run

```bash
# Single benchmark
quorum benchmark --role move-dev --task move-counter

# Full suite — runs all 5, prints per-task scores + mean / median
quorum benchmark --role move-dev
```

The CLI scaffolds a fresh temp project, copies `expected/` if present,
initializes `.quorum/`, creates the standard 4-agent benchmark team
(leader / move-dev doer / evaluator / scribe), writes
`.quorum/teams/benchmark.yaml`, drives the conversation through the
daemon, queries the resulting `evaluations` row, and cleans up.

## Tasks (move-dev)

| Task                | Difficulty | Hits |
|---------------------|------------|------|
| move-counter        | easy       | basic struct + entry fun + init pattern |
| move-escrow         | medium     | shared object capabilities + abort discipline |
| move-fix-abort      | medium     | diagnose + fix a buggy abort condition |
| move-test-coverage  | medium     | test discipline (happy / boundary / authz) |
| move-refactor       | medium     | Move 2024 idiom modernization |

A well-prepared move-dev agent should clear 80+ on `move-counter` and
60+ on the medium-difficulty tasks. Calibration is Track 9 #39's job.
