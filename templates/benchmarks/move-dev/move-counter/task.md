---
name: move-counter
description: Implement a simple counter Move module from scratch.
---

# Move Counter Benchmark

## Goal

Implement a single-owner counter module on Sui Move 2024. The counter is
a shared object that anyone can read and only the owner (holding a
`CounterCap`) can mutate. This is the "Hello World" of Move ownership —
exercise the basic patterns cleanly.

The module should expose:
- `init` — creates a `Counter` shared object with value 0 and transfers
  a `CounterCap` to the publisher.
- `increment(counter, cap, ctx)` — owner-only, increments by 1.
- `decrement(counter, cap, ctx)` — owner-only, aborts if value is 0
  with a stable error code.
- `value(counter)` — public getter, returns the current value.

## Constraints

- Module must compile with `sui move build` cleanly (no warnings).
- All tests under `tests/` must pass with `sui move test`.
- Use Move 2024 idioms: `module pkg::name;` form (single-line),
  `let mut` for mutable bindings, method syntax where applicable,
  `entry` distinct from `public`.
- Capability struct must be named `CounterCap` and held by `key, store`.
- Decrement-below-zero must abort with a `const EUnderflow` error
  constant.

## What to deliver

- `Move.toml` declaring `edition = "2024.beta"` (or newer).
- `sources/counter.move` — the module.
- `tests/counter_tests.move` — at minimum: `init` produces shared
  object and capability, `increment` increases value, `decrement`
  decreases value, `decrement` from zero aborts with `EUnderflow`,
  `value` returns the right number.

The evaluator will score this against the move-dev rubric. Categories
that matter most for this task: Compilation and tests, Move 2024
idioms, Capabilities, Aborts and errors, Test coverage.
