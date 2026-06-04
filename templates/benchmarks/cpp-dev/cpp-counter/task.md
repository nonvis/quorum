---
name: cpp-counter
description: Implement a small bounded counter class in modern C++20 from scratch.
---

# C++ Counter Benchmark

## Goal

Implement a `BoundedCounter` class in modern C++20 from scratch. The counter
holds a non-negative integer with a fixed maximum; callers increment and
decrement it, and both operations refuse to go out of range rather than wrap or
abort. This is the "Hello World" of the C++ bar — exercise the basic patterns
cleanly.

The class should expose:
- A constructor taking the inclusive `max_value` (the counter starts at 0).
- `bool increment()` — increases the value by 1; returns `false` and leaves the
  value unchanged if already at `max_value` (fail closed).
- `bool decrement()` — decreases the value by 1; returns `false` and leaves the
  value unchanged if already at 0 (no underflow).
- `value()` — a `[[nodiscard]]` const getter returning the current value.
- `max_value()` — a `[[nodiscard]]` const getter returning the configured max.

## Constraints

- Must build cleanly under C++20 with `-Wall -Wextra` (no warnings).
- All tests must pass via `ctest`.
- Header uses `#pragma once`; private members carry a trailing underscore.
- `value()` / `max_value()` are `[[nodiscard]] ... const`.
- Use an unsigned integer type (`std::uint64_t`) for the value — no signed
  underflow paths.
- No raw `new` / `delete`; the class owns no heap and follows rule-of-zero.

## What to deliver

- `CMakeLists.txt` declaring `CMAKE_CXX_STANDARD 20`, building the library + a
  test executable, with `enable_testing()` + `add_test`.
- `include/bounded_counter.h` — the class (header-only is fine, or split a
  `src/bounded_counter.cpp`).
- `tests/test_bounded_counter.cpp` — a hand-rolled `assert`-based harness (no
  external test framework) covering at minimum: starts at 0; `increment` raises
  the value; `decrement` lowers it; `increment` at `max_value` returns `false`
  and does not change the value; `decrement` at 0 returns `false` and does not
  change the value; the getters return the right numbers.

The evaluator will score this against the cpp-dev rubric. Categories that matter
most for this task: Compilation and build, Modern C++20 idioms, Memory and
resource safety, Testing.
