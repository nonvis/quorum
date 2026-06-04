---
name: ts-fix-bug
description: Diagnose and fix a real balance-accumulation bug in a TS module, with a Vitest regression.
---

# TS Fix-Bug Benchmark

## Goal

The starter module at `src/balances.ts` tracks per-address token balances
(`bigint`). It compiles and its existing tests pass, but there's a real
correctness bug: under a specific input pattern, balance is silently lost.
Diagnose the bug, fix it, and add a Vitest regression test covering the
previously-failing input.

The bug is a behavioral correctness bug, not stylistic. Preserve the public API
(exported function signatures stable); fix the control flow.

## Constraints

- `npx tsc --noEmit` clean under `strict: true`; all existing tests must still
  pass after the fix (`npx vitest run`).
- Do not change the signatures of the exported functions.
- Existing tests must keep passing — don't delete them.
- Add a regression test that exercises the previously-failing input; it must PASS
  after your fix (the bug was lost balance — the fix makes the total correct).
- Use `bigint` arithmetic throughout (no `number` coercion of token amounts).
- Add a brief comment on the fixed function explaining what was wrong and why the
  fix works.

## What to deliver

- The fixed `src/balances.ts` (control-flow fix).
- `test/balances.test.ts` with the added regression test.
- `npx tsc --noEmit` clean and `npx vitest run` green.

The evaluator will score against the ts-dev rubric. Categories that matter most:
Testing (the regression test is the success signal), Tooling and build, Type
safety and TS rigor (bigint discipline, no `any`).

## Hint

The bug shows up on a **second credit to the same address**. Trace the deposit
path — look at exactly how the per-address amount is written back into the map
when the address already has a balance.
