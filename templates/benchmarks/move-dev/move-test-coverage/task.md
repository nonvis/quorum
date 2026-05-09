---
name: move-test-coverage
description: Write a comprehensive test suite for an under-tested registry module.
---

# Move Test Coverage Benchmark

## Goal

The starter module at `sources/registry.move` is a name registry: users
register short names against their address, can transfer ownership
under the gating capability, and can release names. The module
compiles and the public API is stable, but the test suite is empty.

Write `tests/registry_tests.move` covering:
- **Happy path** for every public / entry function.
- **Boundary inputs** — empty name, max-length name (32 bytes), names
  with non-ASCII bytes, lookup of non-existent name.
- **Authorization-failure tests** with `#[expected_failure(abort_code = ...)]`
  for every capability-gated function (transfer, release).
- **Re-registration** — registering the same name twice aborts; verify.
- **Self-transfer** — transferring a name to its current owner —
  explicitly choose `#[expected_failure]` OR a successful no-op based
  on the source's actual behavior. Read the module first.

## Constraints

- Tests must compile and pass with `sui move test`.
- Use `assert_eq!` for value comparisons, `assert!` only for booleans.
- Use `#[expected_failure(abort_code = E*)]` referencing the named
  error constants from the source (not raw numeric codes).
- Don't modify `sources/registry.move`. Tests only.
- Use `sui::test_scenario` for multi-tx flows (capability transfers).

## What to deliver

- `tests/registry_tests.move` — at minimum 8 tests covering the
  categories above. Group related cases under descriptive function
  names like `test_register_happy`, `test_register_duplicate_aborts`,
  `test_transfer_unauthorized_aborts`, etc.

The evaluator will score against the move-dev rubric. Categories that
matter most for this task: Test coverage (it's the entire task),
Aborts and errors (correct abort code references in
`#[expected_failure]`), Comments and docs (test names should make the
intent obvious without comments).
