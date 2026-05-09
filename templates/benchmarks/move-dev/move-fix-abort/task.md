---
name: move-fix-abort
description: Diagnose and fix a buggy abort condition in a vault module.
---

# Move Fix-Abort Benchmark

## Goal

The starter module at `sources/vault.move` is a simple coin vault: users
deposit a `Coin<SUI>` and later withdraw their balance. The module
compiles, but it aborts on a specific input pattern that should be
allowed. Diagnose the abort, fix it, and add a regression test that
covers the previously-failing input.

The bug is real, not stylistic. The fix should preserve the existing
public API (function signatures stable). Do not paper over the abort
with a try/catch — Move has no such thing; the fix must be in the
control flow.

## Constraints

- Module must compile with `sui move build` cleanly.
- All tests under `tests/` must pass with `sui move test` after your
  fix.
- The fix must use a named `const E*` error constant for any new abort
  paths you introduce.
- Add a regression test that exercises the previously-failing input;
  the test should PASS (not `#[expected_failure]`) — the original bug
  was that the call aborted, the fix is to make it succeed.
- Existing test cases must continue to pass — don't delete tests.
- Do not change function signatures.

## What to deliver

- Modified `sources/vault.move` with the fix.
- Updated `tests/vault_tests.move` adding the regression test.
- A brief one-paragraph note inline as a `///` doc comment on the fixed
  function explaining what was wrong and why the fix works.

The evaluator will score against the move-dev rubric. Categories that
matter most for this task: Compilation and tests (the regression test
is the success signal), Aborts and errors (correct abort code
discipline on any new abort paths), Comments and docs (the inline
explanation of the bug).

## Hint

The bug shows up when a user makes a second deposit. Trace the flow
carefully — the abort is in the deposit path, not withdraw. Look at
how the per-user balance is stored.
