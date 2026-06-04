# cpp-dev synthetic benchmark suite

Five self-contained C++20 tasks the daemon runs through the standard
`leader → cpp-dev → evaluator` team pipeline. Each benchmark exercises a
distinct slice of the cpp-dev rubric (`templates/rubrics/cpp-dev/rubric.md`).

## Layout

```
templates/benchmarks/<role-specialty>/<task-name>/
  task.md            REQUIRED. Frontmatter (name + description) + Goal +
                     Constraints + Deliverables. Body becomes the
                     conversation goal seeded into `quorum converse`.
  expected/          OPTIONAL. Pre-populated starter files (e.g. buggy
                     project to fix, C++03-style project to modernize)
                     copied into the temp project before the agents start.
  evaluator-notes.md OPTIONAL. Task-specific scoring guidance the
                     evaluator may consult alongside the rubric.
```

`expected/` is a starter-file directory, NOT a fixture for output
comparison. The rubric drives correctness assessment; we don't diff files.

All starter projects are **dependency-free**: standard C++20 + CMake only, with
a hand-rolled `assert`/`printf` test harness (no gtest required), so the
benchmark builds anywhere a C++20 toolchain + CMake exist. The agent builds with
`cmake -S . -B build && cmake --build build && ctest --test-dir build`.

## Run

```bash
# Single benchmark
quorum benchmark --role cpp-dev --task cpp-counter

# Full suite — runs all 5, prints per-task scores + mean / median
quorum benchmark --role cpp-dev
```

The CLI scaffolds a fresh temp project, copies `expected/` if present,
initializes `.quorum/`, creates the standard 3-agent benchmark roster
(leader / cpp-dev doer / evaluator), drives the conversation through the daemon,
queries the resulting `evaluations` row, and cleans up.

## Tasks (cpp-dev)

| Task             | Difficulty | Hits |
|------------------|------------|------|
| cpp-counter      | easy       | basic class + RAII + typed return + `[[nodiscard]]` + test from scratch |
| cpp-fix-bug      | medium     | diagnose + fix a real correctness bug + regression test |
| cpp-modernize    | medium     | C++03 → C++20 idiom modernization (raw ptr → smart ptr, out-param → optional, `enum class`) |
| cpp-test-coverage| medium     | test discipline (happy / boundary / failure) for an under-tested component |
| cpp-ledger       | medium     | deterministic, integer-safe, thread-safe balance tracker from scratch (ownership + error + determinism + tests) |

A well-prepared cpp-dev agent should clear 80+ on `cpp-counter` and 60+ on the
medium-difficulty tasks. Calibration is a follow-up benchmarking pass.
