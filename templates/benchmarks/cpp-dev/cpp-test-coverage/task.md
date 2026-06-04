---
name: cpp-test-coverage
description: Write a comprehensive test suite for an under-tested name-registry component.
---

# C++ Test Coverage Benchmark

## Goal

The starter project is a `NameRegistry`: users register a short name (1–32
bytes) against their address, transfer a name they own to a new owner, release a
name they own, and look up a name's owner. The component compiles and its public
API is stable, but the test suite is essentially empty. Write a comprehensive
test suite.

Cover:
- **Happy path** for every public function (`register_name`, `transfer`,
  `release`, `owner_of`).
- **Boundary inputs** — empty name (rejected), 1-byte name (accepted),
  max-length 32-byte name (accepted), 33-byte name (rejected), lookup of a
  non-existent name (absent).
- **Failure cases** for every authorization-gated / precondition-gated path:
  - register a name that is already taken → the `NameTaken` error
  - transfer a name you don't own → the `NotOwner` error
  - transfer / release a name that doesn't exist → the `NotFound` error
  - release a name you don't own → the `NotOwner` error
- **Re-registration** — registering the same name twice fails with `NameTaken`;
  verify the original owner is unchanged.
- **Self-transfer** — transferring a name to its current owner. Read the source
  first and test its **actual** behavior (success no-op vs an error) — do not
  guess.

## Constraints

- Tests must compile under C++20 with `-Wall -Wextra` and pass via `ctest`.
- Assert against the named error values from the component's `enum class` error
  type — not raw integers.
- Do NOT modify the component's header or source. Tests only.
- Tests are deterministic — no sleeps, no order-dependence.
- Use the project's existing hand-rolled `assert`-based harness style (no new
  test-framework dependency).

## What to deliver

- The test file, expanded to at least 10 test functions with descriptive names
  (e.g. `register_happy`, `register_duplicate_fails`, `transfer_unauthorized_fails`,
  `name_too_long_rejected`, `self_transfer_behavior`), wired into `ctest`.

The evaluator will score against the cpp-dev rubric. Categories that matter
most: Testing (it's the entire task), Error handling (asserting the correct
named error values), Comments and docs (descriptive test names that make intent
obvious without comments).
