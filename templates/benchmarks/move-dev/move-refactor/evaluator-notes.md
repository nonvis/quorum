# move-refactor — evaluator notes

This task is exclusively about Move 2024 idiom adoption. Items to
weight more heavily:

- **Move 2024 idioms** — the entire purpose of the benchmark. Score
  every item in this category strictly. Specifically check:
  - Module declaration is `module pkg::name;` form, not braces.
  - `let mut` everywhere a binding is later reassigned. The starter
    file has `let total = 0; ...; total = total + ...;` pattern that
    requires `let mut total`.
  - `vector[a, b, c]` literal where appropriate (especially the
    `recent_deposits` slice).
  - Method syntax: `t.balance()` not `balance(&t)` for getters; the
    `coin::value(&c)` calls become `c.value()` where natural.
  - Getter naming: `balance(&t)` instead of `get_balance(&t)`.
  - `public(package) fun` for `rotate_admin` (it's only used
    within-package).
- **Compilation and tests** — regression-free is the floor. If the
  refactor breaks the existing test, score as fundamental failure
  regardless of how clean the new idioms look.

Items that matter LESS for this task:
- Capabilities — the existing `AdminCap` is fine; no new capability
  design happening.
- Aborts and errors — the existing error constants are reasonable;
  refactor isn't expected to add new abort paths.
- Test coverage — the existing tests stay; agent isn't asked to add
  new tests.
- Comments and docs — the agent should preserve existing comments;
  fresh prose isn't expected for a pure refactor.

Watch for: agents that "improve" beyond what the task asked. Adding
new abort paths, splitting functions, or renaming public APIs are
out-of-scope; the task explicitly says preserve behavior. Score down
in Test coverage if "improvements" break the existing suite.
