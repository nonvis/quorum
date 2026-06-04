# cpp-ledger — evaluator notes

Medium-difficulty flagship benchmark. A well-prepared cpp-dev agent should clear
60+. This task is the acid test for the structural rules; score the following
categories strictly.

- **Error handling — typed + fail-closed** — the heart of the task.
  - Mutating ops MUST return a typed reason (`std::optional<LedgerError>` or a
    `LedgerError` enum class), not a bare `bool`/`-1` and not a thrown exception
    for these expected conditions. A bare `bool` here IS a violation (multiple
    distinct failure causes — InsufficientFree vs InsufficientLocked vs Overflow
    — collapse to one bit), unlike cpp-counter.
  - **Fail-closed is the trap.** Verify by reading the code AND the tests: a
    `withdraw`/`lock` that subtracts from free *before* checking sufficiency, or
    a `deposit` that mutates before the overflow check, leaves torn state on the
    failure path. The "state unchanged after a failed op" tests are what catch
    this — if they're missing, dock both Error handling (fail-closed) and
    Testing (failure path).

- **Concurrency and determinism** — these items are **in scope** here (this is
  replicated-state-machine-shaped code), NOT N/A.
  - `std::map`, never `std::unordered_map`, for the per-account state. An
    `unordered_map` breaks the determinism guarantee even if the snapshot is
    sorted afterward — score the determinism item down and call it out.
  - Integer-only math; any `float`/`double` in balance handling is a hard miss.
  - Shared state guarded by a mutex via RAII. A `shared_mutex` with
    `shared_lock` on the read paths is the ideal; a plain `mutex` is acceptable.
    Manual `lock()`/`unlock()` around code that can early-return is a finding.
  - The determinism/replay test (same ops → identical snapshot on two ledgers)
    is mandatory — its absence fails the "consensus determinism test" item.

- **Memory and resource safety** — no raw owning pointers; the ledger owns its
  `std::map` by value (rule-of-zero). `std::array<uint8_t,32>` for the address,
  not a C array or `std::vector`.

Acceptable latitude:
- Either `std::optional<LedgerError>` (nullopt = success) or an enum with an
  explicit `Ok` is fine — don't prefer one over the other.
- Header-only or split `.cpp` both fine.
- Overflow detection via `a > UINT64_MAX - b` or via `__builtin_add_overflow` /
  `<limits>` are all acceptable; what matters is that overflow is checked and
  fails closed.

Items that matter LESS:
- Comments and docs — value the threading-contract comment (which mutex guards
  what) but don't expect prose beyond that.
- Performance — correctness and determinism dominate; don't dock for using
  `std::map` over a faster structure (ordered iteration is required here).
