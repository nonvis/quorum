---
name: ts-types
description: Replace `any` with strict types and discriminated-union narrowing, preserving behavior.
---

# TS Types Benchmark

## Goal

The starter module at `src/events.ts` parses Sui-style on-chain event payloads. It
works, but it's typed with `any` throughout and narrows by ad-hoc property
checks. Replace the loose typing with a proper **discriminated union** and narrow
on the tag field — no `any`, no behavior change.

## Constraints

- `npx tsc --noEmit` must pass under `strict: true` (it already does — keep it
  that way).
- **No `any`** remaining in `src/events.ts` (explicit `any`, `as any`, or implicit).
  Model the event payloads as a discriminated union (a `type` per variant with a
  shared literal `kind`/`type` tag) and narrow with `Extract<...>` or a `switch`
  on the tag.
- Do not use non-null assertions (`!`) or `as` casts to dodge the types — narrow
  properly.
- Behavior must not change: the existing tests in `test/events.test.ts` must keep
  passing (`npx vitest run`). Update the tests' types if needed, but not their
  assertions.
- Public function signatures stay stable except for tightening `any` → the real
  types (callers in the test should still compile).

## What to deliver

- `src/events.ts` re-typed: a discriminated union for the event payloads, the
  parser returning the precise type, narrowing without `any`/casts.
- `test/events.test.ts` updated only as needed to compile against the new types
  (same assertions).
- `npx tsc --noEmit` clean and `npx vitest run` green.

The evaluator will score against the ts-dev rubric. Categories that matter most:
Type safety and TS rigor (the entire task), Tooling and build (clean typecheck +
green tests).
