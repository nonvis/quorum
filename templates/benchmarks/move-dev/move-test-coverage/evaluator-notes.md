# move-test-coverage — evaluator notes

This task is exclusively about test discipline. Items to weight more
heavily:

- **Test coverage** — every public/entry function should have a happy
  path test AND an authorization-failure test where applicable.
  Boundary cases (empty name, max-length, non-ASCII) on top. Score
  this category strictly; this is what the benchmark is measuring.
- **Aborts and errors — `#[expected_failure(abort_code = ...)]`** —
  the abort code references must be by named constant, not raw
  numeric values. A `#[expected_failure]` without `abort_code = ...`
  is a sloppy test — score down.
- **Compilation and tests** — full pass under `sui move test` is the
  binary correctness floor. Don't give partial credit if some tests
  fail; one red test means the agent's coverage understanding is
  wrong.

Items that matter LESS:
- Move 2024 idioms — the source is provided, the agent isn't writing
  module code. Tests should still use `let mut` correctly.
- Capabilities — the agent is exercising existing capabilities, not
  designing new ones.
- Comments and docs — for tests, descriptive function names beat
  prose comments. Don't penalize terse tests with explanatory names.

Self-transfer behavior is intentionally underspecified to test whether
the agent reads the source first or just guesses.
