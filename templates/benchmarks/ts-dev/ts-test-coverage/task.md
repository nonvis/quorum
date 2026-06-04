---
name: ts-test-coverage
description: Write a comprehensive Vitest suite for an under-tested name-registry module.
---

# TS Test Coverage Benchmark

## Goal

The starter module at `src/registry.ts` is a `NameRegistry`: register a short name
(1–32 chars) to an owner address, transfer a name you own, release a name you own,
and look up a name's owner. It compiles and the public API is stable, but the test
suite is essentially empty. Write a comprehensive Vitest suite.

Cover:
- **Happy path** for every exported function (`registerName`, `transfer`,
  `release`, `ownerOf`).
- **Boundary inputs** — empty name (rejected), 1-char (accepted), 32-char
  (accepted), 33-char (rejected), lookup of a non-existent name (returns
  `undefined`/absent).
- **Failure cases** for every gated path, asserting the specific returned error:
  - register an already-taken name → `NameTaken`
  - transfer a name you don't own → `NotOwner`
  - transfer / release a name that doesn't exist → `NotFound`
  - release a name you don't own → `NotOwner`
- **Re-registration** — registering the same name twice fails with `NameTaken`;
  the original owner is unchanged.
- **Self-transfer** — transferring a name to its current owner. Read the source
  first and test its **actual** behavior (success no-op vs an error) — don't guess.

## Constraints

- Tests compile under `strict: true` (`npx tsc --noEmit` clean) and pass
  (`npx vitest run`).
- Assert against the module's exported error values/enum — not bare strings or
  truthiness.
- Do NOT modify `src/registry.ts`. Tests only.
- Tests deterministic — no timers, no shared mutable state across cases.
- No `any` in the test file.

## What to deliver

- `test/registry.test.ts` expanded to at least 10 `it(...)` cases with descriptive
  names, wired into the `test` script.

The evaluator will score against the ts-dev rubric. Categories that matter most:
Testing (the entire task), Type safety (asserting the typed error values),
Tooling and build (clean typecheck + green tests).
