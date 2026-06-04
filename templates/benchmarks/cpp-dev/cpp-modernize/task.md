---
name: cpp-modernize
description: Modernize a C++03-style component to idiomatic C++20 while preserving behavior.
---

# C++ Modernize Benchmark

## Goal

The starter project is a working **C++03-style** key/value registry: it
compiles, but uses dated idioms throughout — raw `new`/`delete` ownership,
output-parameter returns, `NULL`, `typedef`, unscoped `enum`, `get_`-prefixed
getters, and manual index loops. Refactor it to idiomatic **C++20** while
preserving behavior exactly.

Specifically, modernize:
- **Ownership**: raw `new`/`delete` and owning raw pointers → `std::unique_ptr`
  / owning containers (no manual `delete`, rule-of-zero/five as appropriate).
- **Returns**: output-parameter lookups (`bool lookup(key, Value* out)`) →
  `std::optional<Value>`.
- **Enums**: unscoped `enum Status { OK, ... }` → `enum class Status { Ok, ... }`.
- **Null**: `NULL` → `nullptr`.
- **Aliases**: `typedef` → `using`.
- **Getters**: drop the `get_` prefix (`get_count()` → `count()`); mark pure
  queries `[[nodiscard]] ... const`.
- **Loops**: manual index `for (size_t i = 0; ...)` over a container → range-for
  or a standard algorithm; verbose iterator types → `auto` / structured
  bindings.
- **Build**: bump `CMAKE_CXX_STANDARD` to `20`.

## Constraints

- Behavior MUST NOT change — same public operations, same results, same error
  conditions. Update call sites + tests to match any signature change you make
  (e.g. switching an out-param to `std::optional`).
- Must build cleanly under C++20 with `-Wall -Wextra` (no warnings); all tests
  must continue to pass via `ctest`.
- Don't add new functionality. Modernize only.
- No raw `new`/`delete` remaining; no owning raw pointers; no `NULL`; no bare
  `enum`; no `typedef`; no `get_` getters.

## What to deliver

- `CMakeLists.txt` with `CMAKE_CXX_STANDARD 20`.
- The modernized header(s) and source.
- The test file updated to the new signatures (e.g. `std::optional` instead of
  the out-param), still covering the same cases and still passing.

The evaluator will score against the cpp-dev rubric. Categories that matter
most: Modern C++20 idioms (this is the entire task), Memory and resource safety
(the `new`/`delete` → smart-pointer conversion), Compilation and build
(regression-free behavior).
