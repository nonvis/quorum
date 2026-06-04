# cpp-counter — evaluator notes

This is the easy benchmark. A well-prepared cpp-dev agent should clear 80+.
Items to weight more heavily than the default rubric suggests:

- **Compilation and build** — for a class this small, "builds cleanly" + "all
  tests pass" carry effectively the entire correctness signal. Penalize hard if
  either fails; a green build (no `-Wall -Wextra` warnings) + green tests is the
  floor.
- **Modern C++20 idioms** — `#pragma once`, `[[nodiscard]]` on the getters,
  trailing-underscore member, unsigned value type, rule-of-zero. These are the
  point of the task; score them strictly.
- **Memory and resource safety** — there is no heap here; the correct answer
  owns nothing and needs no special members. An agent that introduces `new`/raw
  pointers for a value type has missed the bar.

Acceptable latitude:
- **`bool` return on increment/decrement is correct here**, not a rubric
  violation. The "typed value, not a bare bool" rule targets failures with
  *multiple* causes; increment/decrement each have a single binary outcome
  (in-range vs at-bound), so `bool` loses no information. Do not dock for it.
- **Header-only is fine.** A split `.cpp` is also fine. Don't prefer one.

Items that matter LESS for this task:
- Concurrency and determinism — single-threaded value type; the determinism
  items are N/A. Don't expect a mutex.
- Comments and docs — easy benchmark; terse code with clear names is fine.
  Don't penalize thin doc comments.
