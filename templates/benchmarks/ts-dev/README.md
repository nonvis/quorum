# ts-dev synthetic benchmark suite

Five self-contained TypeScript tasks the daemon runs through the standard
`leader → ts-dev → evaluator` team pipeline. Each exercises a distinct slice of
the ts-dev rubric (`templates/rubrics/ts-dev/rubric.md`).

## Layout

```
templates/benchmarks/<role-specialty>/<task-name>/
  task.md            REQUIRED. Frontmatter (name + description) + Goal +
                     Constraints + Deliverables. Body becomes the
                     conversation goal seeded into `quorum converse`.
  expected/          OPTIONAL. Pre-populated starter files (e.g. loosely-typed
                     code to tighten, a buggy module to fix) copied into the
                     temp project before the agents start.
  evaluator-notes.md OPTIONAL. Task-specific scoring guidance.
```

`expected/` is a starter-file directory, NOT a fixture for output comparison.
The rubric drives correctness; we don't diff files.

## Dependency posture

**Dependency-light by default.** Four tasks need only the small, reliable dev
toolchain (`typescript` + `vitest`); the agent runs:

```bash
pnpm install && npx tsc --noEmit && npx vitest run
```

One task (`ts-ptb`) installs the real `@mysten/sui` SDK to exercise PTB
construction + execution typing against actual SDK types. No task talks to
mainnet; the chain is mocked or the SDK is used for typecheck only.

**Known v1 gap:** the **dApp Kit frontend** surface (rubric category "dApp Kit
frontend") is *not* benchmark-covered in v1 — a React + `@mysten/dapp-kit-react`
starter would require the heavier install we deliberately avoided. It is covered
by the `sui-frontend` skill + the rubric; a frontend e2e benchmark is a future
addition. Calibration should account for this.

## Run

```bash
# Single benchmark
quorum benchmark --role ts-dev --task ts-types

# Full suite — runs all 5, prints per-task scores + mean / median
quorum benchmark --role ts-dev
```

## Tasks (ts-dev)

| Task             | Difficulty | Deps | Hits |
|------------------|------------|------|------|
| ts-ptb           | easy       | @mysten/sui | PTB construction + signing + execution typing |
| ts-types         | medium     | light | strict typing: kill `any`, narrow SDK unions with `Extract<>` |
| ts-fix-bug       | medium     | light | diagnose + fix a real logic bug + Vitest regression |
| ts-modernize     | medium     | light | legacy TS → modern ESM/strict idioms (preserve behavior) |
| ts-test-coverage | medium     | light | Vitest discipline (happy / boundary / failure) |

A well-prepared ts-dev agent should clear 80+ on `ts-ptb` and 60+ on the
medium tasks. Calibration is a follow-up benchmarking pass.
