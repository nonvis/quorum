---
name: cpp-ledger
description: Implement a deterministic, integer-safe, thread-safe balance ledger in modern C++20 from scratch.
---

# C++ Ledger Benchmark

## Goal

Implement a `BalanceLedger` in modern C++20 from scratch — an in-memory,
per-account balance tracker of the kind that sits inside a replicated state
machine. Correctness, deterministic iteration, integer-exactness, and clean
error reporting all matter; this is the flagship cpp-dev task.

Each account is identified by a 32-byte address (`std::array<std::uint8_t, 32>`)
and holds a free balance and a locked balance, both `std::uint64_t`. The ledger
exposes:

- `deposit(addr, amount)` — adds `amount` to the account's free balance.
  Returns a typed failure if the addition would overflow `uint64_t`.
- `lock(addr, amount)` — moves `amount` from free to locked. Fails if the
  account's free balance is insufficient.
- `release(addr, amount)` — moves `amount` from locked back to free. Fails if
  locked is insufficient.
- `withdraw(addr, amount)` — removes `amount` from free. Fails if free is
  insufficient.
- `free_balance(addr)` / `locked_balance(addr)` — `[[nodiscard]]` const reads;
  return 0 for an unknown account.
- `snapshot()` — returns the full state in a **deterministic order** (ascending
  by address) so two ledgers that received the same operations produce an
  identical snapshot.

## Constraints

- Must build cleanly under C++20 with `-Wall -Wextra` (no warnings); all tests
  pass via `ctest`.
- **Typed errors, not sentinels.** Mutating ops return a result that names the
  reason — e.g. `std::optional<LedgerError>` (`nullopt` = success) or a
  `LedgerError` enum class with values like `Ok`, `InsufficientFree`,
  `InsufficientLocked`, `Overflow`. Do not signal failure with a bare `-1` or by
  throwing for these expected conditions.
- **Fail closed.** A mutation that cannot fully succeed must leave the account
  exactly as it was — no partial application (e.g. don't subtract from free
  before confirming the overflow/underflow check passes).
- **Determinism.** Keyed state uses an **ordered** container (`std::map`, never
  `std::unordered_map`) so iteration / snapshot order is identical across
  replicas. Integer-only math — no `float`/`double` anywhere.
- **Thread-safe.** Guard shared state with a mutex held via RAII
  (`std::lock_guard` / `std::shared_mutex` + `std::shared_lock` for reads).
  Document which mutex guards what.
- Header uses `#pragma once`; private members trailing-underscore; `enum class`
  for the error type; `[[nodiscard]]` on queries.
- No raw `new` / `delete`.

## What to deliver

- `CMakeLists.txt` (`CMAKE_CXX_STANDARD 20`, library + test executable,
  `enable_testing()` + `add_test`).
- `include/balance_ledger.h` (+ optional `src/balance_ledger.cpp`).
- `tests/test_balance_ledger.cpp` — a hand-rolled `assert`-based harness
  covering at minimum:
  - deposit then read; lock moves free→locked; release moves locked→free;
    withdraw reduces free
  - **failure paths**: lock/withdraw beyond free → `InsufficientFree` and state
    unchanged; release beyond locked → `InsufficientLocked` and state unchanged;
    deposit that overflows `uint64_t` → `Overflow` and state unchanged
  - **boundaries**: unknown account reads 0; deposit up to `UINT64_MAX`; zero-
    amount ops
  - **determinism**: apply the same set of operations to two ledgers in the same
    order and assert `snapshot()` is byte-for-byte identical

The evaluator will score against the cpp-dev rubric. Categories that matter
most: Error handling (typed + fail-closed), Concurrency and determinism
(ordered map, integer-only, RAII locking), Memory and resource safety, Testing.
