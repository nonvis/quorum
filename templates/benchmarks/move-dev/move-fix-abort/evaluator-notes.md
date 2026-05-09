# move-fix-abort — evaluator notes

This task is bug-diagnosis-then-fix. Items to weight more heavily:

- **Compilation and tests** — the regression test for the
  previously-failing input is the primary success signal. If the
  agent's regression test still uses `#[expected_failure]` (treating
  the bug as desired behavior), that's a failure of comprehension —
  score "package builds" + "all tests pass" carefully against the
  actual fix, not the agent's interpretation.
- **Aborts and errors — fail-closed** — does the fix preserve the
  `EZeroDeposit` check (which IS correct fail-closed behavior)? If the
  agent removes that assert in the course of fixing the unrelated bug,
  dock heavily.
- **Comments and docs — non-obvious math / abort risk** — the inline
  `///` comment on the fixed function explaining the bug and the fix
  is mandatory per the task. Missing → dock the relevant rubric item.

The bug: `table::add` aborts on duplicate-key insert. Fix is to check
`table::contains` and `table::borrow_mut`+`balance::join` on
re-deposit, only `table::add` on first deposit. A correct fix should
not require new error constants — it's a control-flow fix.

Items that matter LESS:
- Move 2024 idioms — the starter file already uses 2024 idioms; the
  agent isn't being asked to refactor.
- Test coverage — happy path + regression test is enough; don't expect
  full attacker-controlled-input sweep on this benchmark.
