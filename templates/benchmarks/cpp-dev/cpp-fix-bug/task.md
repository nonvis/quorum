---
name: cpp-fix-bug
description: Diagnose and fix a real correctness bug in a coin-vault component, with a regression test.
---

# C++ Fix-Bug Benchmark

## Goal

The starter project is a simple in-memory coin vault: users `deposit` an amount
under their account id and later `withdraw` it; `balance_of` reads the current
balance. The project compiles and its existing tests pass, but there is a real
correctness bug: under a specific input pattern, money silently goes missing.
Diagnose the bug, fix it, and add a regression test that covers the
previously-failing input.

The bug is a behavioral correctness bug, not stylistic. The fix must preserve
the existing public API (function signatures stable) and must be a control-flow
fix — do not change the API to paper over it.

## Constraints

- Must build cleanly under C++20 with `-Wall -Wextra`; all existing tests must
  still pass via `ctest` after your fix.
- Do not change the signatures of `deposit`, `withdraw`, or `balance_of`.
- Existing test cases must continue to pass — don't delete tests.
- Add a regression test that exercises the previously-failing input; the test
  should PASS after your fix (the bug was lost funds — the fix is to make the
  balance correct).
- If your fix introduces any new failure path (e.g. overflow), report it through
  the component's existing typed-error mechanism — do not throw for an expected
  condition and do not return a magic sentinel.
- Add a brief `///` doc comment on the fixed function explaining what was wrong
  and why the fix works.

## What to deliver

- The fixed source file with the control-flow fix.
- The updated test file adding the regression test for the previously-failing
  input.
- The inline `///` explanation on the fixed function.

The evaluator will score against the cpp-dev rubric. Categories that matter
most: Compilation and build (the regression test is the success signal), Error
handling (fail-closed; correct typed-error use on any new path), Comments and
docs (the inline explanation of the bug).

## Hint

The bug shows up when an account makes a **second deposit**. Trace the deposit
path carefully — look at exactly how a new amount is written into the per-account
map when the account already has a balance.
