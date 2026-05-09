---
name: move-refactor
description: Modernize a verbose pre-Move-2024 module to use 2024 idioms.
---

# Move Refactor Benchmark

## Goal

The starter module at `sources/treasury.move` is a working pre-Move-2024
treasury: it compiles under the legacy edition, but uses old syntax
throughout. Refactor it to Move 2024 idioms while preserving behavior
exactly.

Specifically, modernize:
- Module declaration: `module pkg::name { ... }` braces → `module pkg::name;` form.
- Mutable bindings: `let x = ...` for things later reassigned → `let mut x = ...`.
- `vector::empty<T>()` + `push_back` chains → `vector[a, b, c]` literal.
- `vector::borrow(v, i)` / `vector::borrow_mut(v, i)` chains → method
  syntax (`v.borrow(i)`, `v[i]` where appropriate).
- First-arg-typed function calls → method syntax (`coin::value(&c)` →
  `c.value()`).
- Internal helper visibility: `public fun helper(...)` only used
  within-package → `public(package) fun helper(...)`.
- Getters: `get_balance(&t)` → `balance(&t)` (drop the `get_` prefix);
  mutable getters end in `_mut`.
- `Move.toml` edition bumped to `2024.beta` (or newer).
- Test file mirrors module idiom updates.

## Constraints

- Behavior MUST NOT change. Same public API, same abort conditions,
  same error codes, same effects on shared / owned state.
- Must compile under `sui move build` cleanly with `edition = "2024.beta"`.
- All existing tests under `tests/` must continue to pass.
- Don't add new functionality. Refactor only.
- Don't change struct field names or layouts (would break existing
  off-chain readers).

## What to deliver

- `Move.toml` with `edition = "2024.beta"`.
- Refactored `sources/treasury.move`.
- Refactored `tests/treasury_tests.move` (update tests to use 2024
  idioms too — `let mut` where applicable, method syntax where natural).

The evaluator will score against the move-dev rubric. Categories that
matter most for this task: Move 2024 idioms (this is the entire
task), Compilation and tests (regression-free behavior).
