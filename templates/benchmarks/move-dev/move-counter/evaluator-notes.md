# move-counter — evaluator notes

This is the easy benchmark. A well-prepared move-dev agent should clear
80+. Items to weight more heavily than the default rubric suggests:

- **Compilation and tests** — for a simple counter, "package builds" +
  "all tests pass" carry effectively the entire correctness signal.
  Penalize hard if either fails; a green compile + green tests is the
  floor.
- **Capabilities** — the only authorization mechanism in this task. A
  module that gates `increment` / `decrement` via `ctx.sender()` checks
  rather than `&CounterCap` is fundamentally wrong, even if it
  superficially works. Score the Capability category items strictly.
- **Aborts and errors** — the underflow path is the only abort surface.
  If `EUnderflow` isn't a `const` with a stable numeric value, or the
  test for the underflow path is missing, dock the relevant items.

Items that matter LESS for this task:
- Transfer semantics — minimal here; a single `transfer::transfer` and a
  `share_object` cover it.
- Comments and docs — easy benchmark, terse code is fine; don't
  penalize hard for thin module-level docs.
