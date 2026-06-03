# Cartographer synthetic benchmark suite

Synthetic tasks for the `cartographer` knower — a thinker (analyst, read-only)
that indexes a workspace's LAYOUT (what each top-level folder contains, where
the key files live) and serves orientation lookups. Each task ships a small
fixture workspace in `expected/`; the daemon runs the standard
`leader → cartographer → evaluator` pipeline and the evaluator scores
the produced index against the cartographer rubric
(`templates/rubrics/cartographer/rubric.md`).

## Layout

```
templates/benchmarks/cartographer/<task-name>/
  task.md            REQUIRED. Frontmatter + Goal + Constraints + Deliverables.
  expected/          The fixture WORKSPACE (a couple of folders + manifests /
                     README). Copied into the temp project before agents start.
                     This is the ground truth the index is scored against —
                     NOT an output to diff.
  evaluator-notes.md The ground-truth layout (expected components + key files +
                     lookup answers) the evaluator scores against.
```

The index is open-form, so there is no rigid `expected/` output file. The
fixture's actual filesystem layout is the ground truth; the evaluator walks the
index against it with Read / Grep / Glob.

## Run

```bash
quorum benchmark --role cartographer --task cartographer-polyglot
quorum benchmark --role cartographer            # all tasks
```

## Tasks

| Task                  | Difficulty | Hits |
|-----------------------|------------|------|
| cartographer-polyglot | easy       | coverage + content + key-file location; honors CLAUDE.md |
| cartographer-find     | easy       | lookup structure + "where is X" served from the index |

A well-prepared cartographer should clear 80+ on both (the bar is ~75%
mechanically checkable against the filesystem).
