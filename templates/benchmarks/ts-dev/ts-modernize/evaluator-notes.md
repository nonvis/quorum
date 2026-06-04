# ts-modernize — evaluator notes

Exclusively about modern-TS idiom adoption with behavior preserved. Items to
weight more heavily:

- **Type safety and TS rigor** + **Structure and conventions** — the point of the
  task. Verify ALL of these are gone from the result (grep):
  - `var` → `const`/`let`
  - `==` / `!=` → `===` / `!==`
  - `any` → real types
  - `Promise.then`/`.catch` chains → `async`/`await` + `try/catch`
  - index `for (var i ...)` → array methods / `for...of`
  - `namespace` → ESM named exports
  - `export default` → named exports
  A residual of any one is a miss on the corresponding item.
- **Tooling and build** — regression-free is the floor: `tsc --noEmit` clean +
  `vitest run` green. If the refactor changes an output or breaks a test, that's a
  fundamental failure regardless of how modern the result reads.

Acceptable latitude:
- `for...of` vs `.map`/`.reduce` — either is fine where it preserves behavior;
  don't insist on one.
- Whether a helper becomes an arrow `const fn = () =>` or a `function` — both are
  modern; don't dock.

Watch for: agents that "improve" beyond the task — adding features, new error
paths, or changing outputs is out of scope. The task says preserve behavior.
Score down Tooling/build if "improvements" break the suite.

Items that matter LESS:
- Sui SDK correctness / dApp Kit — N/A.
- Testing breadth — existing tests stay; the agent updates only the import line.
