---
name: cpp-dev
version: v1
---

# Rubric: cpp-dev (v1)

Sources: the `cpp-code-quality` skill (this repo), the C++ Core Guidelines
(https://isocpp.github.io/CppCoreGuidelines/), cppreference C++20, and the
conventions distilled from two production C++20 systems codebases — one
host-constrained storage engine (2-space / Allman, return-code + framework
logging, C++20 via Ceph Reef — the SAL `int`/`-errno` + framework logging are host-mandated, not the standard) and one greenfield low-latency
matching/consensus engine (4-space / K&R, `std::optional`/`std::variant` errors,
C++20, BFT-deterministic state machine). The rubric encodes what those two
**agree on** (ownership, RAII, typed errors, determinism, tests) as hard rules,
and treats where they **diverge** (indent/brace cosmetics, the error-reporting
idiom) as evaluator judgment.

Calibration intent:
- **C++20 is the baseline** for new code. An older standard PASSes only when the
  host/framework build externally pins it AND the code still uses the modern
  facilities available to it (judgment call — document the reasoning).
- **Cosmetics and the error-reporting idiom are scored as "consistent with the
  surrounding code," not against a fixed value.** This specialty is portable
  across host-constrained and greenfield projects.
- **Determinism items are N/A** for code that is genuinely not replicated /
  consensus / state-machine logic — say so; don't fail it for absent BFT rules.
- **Stub-only test files default to FAIL** (insufficient evidence), not N/A.

The `evaluator` agent reads this file, walks each item, and emits per-item
pass/fail in the EVALUATION block. Categories are documentation; per-item `(W)`
weights drive scoring.

## Compilation and build (weight 20)
- [ ] (6) Builds cleanly with the project's canonical build target (CMake/Make), no errors
- [ ] (6) All tests pass under the project's test target (`ctest`, `make test-unit`, or equivalent)
- [ ] (4) No new compiler warnings introduced under `-Wall -Wextra`
- [ ] (4) C++20 (or newer) is the standard. PASS for `CMAKE_CXX_STANDARD` ≥ 20. For C++17, PASS **only** when the standard is externally pinned by the host/framework build and the code still uses the modern facilities available to it (judgment call — document). FAIL for an unpinned new project on C++14 or older.

## Modern C++20 idioms (weight 18)
- [ ] (3) Every header opens with `#pragma once` (not hand-rolled include guards)
- [ ] (4) Heap ownership held in smart pointers; **no raw `new` / `delete`** and no owning raw pointers
- [ ] (3) Expected, may-be-absent, or multi-value results use `std::optional` / `std::variant` / a named result struct — not magic sentinels or out-parameters
- [ ] (2) Fallible / pure-query functions marked `[[nodiscard]]`
- [ ] (2) Enumerations are `enum class`, never bare `enum`
- [ ] (2) Modern facilities used where natural: `std::span` for byte views, structured bindings, `std::array` for fixed-size data, nested-namespace form
- [ ] (2) Cosmetics (indent width, brace style, constant-casing) are **consistent with the surrounding file** — new code does not introduce a different style mid-file. Judgment call; this is not pinned to a single value.

## Memory and resource safety (weight 16)
- [ ] (4) No owning raw pointers; raw pointers appear only as documented non-owning borrows of objects assumed to outlive the holder
- [ ] (4) Every resource (locks, threads, handles, sockets) is RAII-managed — `lock_guard`/`unique_lock`, threads joined in destructor/`stop()`, no manual `lock()`/`unlock()` straddling code that can early-return or throw
- [ ] (3) Resource-owning types that must not be shallow-copied `= delete` copy (and move where appropriate); value types follow rule-of-zero
- [ ] (3) No manual `new`/`delete`; allocation goes through `make_unique`/`make_shared` or owning containers
- [ ] (2) Sensitive material (keys, secrets) is cleansed on destruction (e.g. `OPENSSL_cleanse`). N/A if the unit handles no secrets.

## Error handling (weight 15)
- [ ] (4) Exceptions reserved for **truly exceptional** conditions (construction/parse/crypto), not expected business-flow rejections
- [ ] (4) Expected failures return a **typed** value (`std::optional`, `std::variant`, status enum, or result struct) that preserves the reason — not a bare `bool`/`-1` that loses it. The reporting idiom (return-code vs optional/variant) follows the surrounding layer's convention; do not mix two idioms in one layer.
- [ ] (4) Fail closed — inputs are validated **before** any state mutation; no half-applied multi-step mutation that aborts mid-way leaving torn state
- [ ] (3) Errors are never silently swallowed — a failed return is logged or propagated; no discarding a `[[nodiscard]]` result without handling

## Concurrency and determinism (weight 13)
- [ ] (4) Shared mutable state is guarded by a lock held via RAII; atomics are used for flags/counters/signaling, not as a substitute for guarding a container
- [ ] (3) No non-thread-safe resource shared across threads without synchronization — prefer per-worker owned resources where it removes a lock
- [ ] (4) **Determinism** (for replicated / consensus / state-machine code): ordered containers (`std::map`, never `unordered_map`) for keyed state; no `system_clock::now()` reads inside a state mutation (timestamps come from the log/input); integer-only math for balances/prices (no `float`/`double`). **N/A** if the code is genuinely not replicated/consensus logic — state that explicitly.
- [ ] (2) The threading contract is documented — which mutex guards what, and the lifetime assumption for any injected/borrowed pointer

## Testing (weight 10)

> **Default rule:** a test file that exists but contains 0 functions exercising the unit (stub-only) defaults to **FAIL** for these items. N/A is reserved for a unit that genuinely has nothing testable.

- [ ] (3) Every new/modified component has a test file wired into the project's test runner
- [ ] (3) Coverage includes the happy path, the expected-failure/rejection path, and boundaries (zero, max `uint64_t`, empty input, max-length key)
- [ ] (2) Tests are deterministic and use the project's existing framework consistently (gtest or a hand-rolled `assert`/`printf` harness — not two at once); no flaky sleeps or order-dependence
- [ ] (2) Consensus/state-machine code has a determinism/replay test (same batch → identical state hash across replicas). **N/A** for non-replicated logic.

## Comments and docs (weight 8)
- [ ] (3) Non-obvious concerns are commented — threading/lock ownership, lifetime assumptions, invariants, overflow/underflow risk — not restatements of the code
- [ ] (3) Public header functions carry a doc comment stating intent and failure/abort conditions
- [ ] (2) A new hard-won API gotcha / anti-pattern is captured in the project's `CLAUDE.md` (or convention doc), not left to be re-learned. N/A if the change surfaced no such gotcha.
