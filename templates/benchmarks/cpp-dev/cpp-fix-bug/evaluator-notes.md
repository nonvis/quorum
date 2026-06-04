# cpp-fix-bug — evaluator notes

This task is bug-diagnosis-then-fix. Items to weight more heavily:

- **Compilation and build** — the regression test for the previously-failing
  input is the primary success signal. If the agent's regression test does not
  actually exercise a *second deposit to the same account* (the bug's trigger),
  the fix is unverified — score "all tests pass" against the real bug, not the
  agent's interpretation.
- **Error handling — fail-closed** — does the fix preserve the existing
  zero-amount / insufficient-funds checks? If the agent removes a valid guard in
  the course of fixing the unrelated deposit bug, dock heavily. If the fix adds
  an overflow path on re-deposit, it must use the component's existing typed
  error, not a throw or sentinel.
- **Comments and docs** — the inline `///` comment on the fixed function
  explaining the bug and the fix is mandatory per the task. Missing → dock the
  relevant rubric item.

The bug: `deposit` writes the amount with `std::map::insert`, which is a **no-op
when the key already exists** — so a second deposit to an existing account is
silently dropped (the account keeps its first balance; the new funds vanish).
The existing tests only ever deposit once per account, so they pass and hide it.

A correct fix updates the existing entry — `balances_[id] += amount` (with an
overflow check), or `find` + add — only `insert`-ing fresh on first deposit. It
should NOT require changing the public API; it's a control-flow fix.

Items that matter LESS:
- Modern C++20 idioms — the starter already uses modern idioms; the agent isn't
  asked to refactor, only to fix.
- Concurrency and determinism — single-threaded component; those items are N/A.
- Testing breadth — happy path (already present) + the second-deposit
  regression test is enough; don't expect a full input sweep on this benchmark.
