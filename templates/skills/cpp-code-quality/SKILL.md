---
name: cpp-code-quality
description: Analyzes modern C++ (C++20) code against a production systems-quality checklist — build hygiene, headers, naming, modern idioms, memory/RAII, error handling, concurrency & determinism, testing. Use when reviewing or writing C++, checking C++20 compliance, or analyzing a C++ component for quality. Activates when working with .cpp/.cc/.h/.hpp files or CMakeLists.txt.
---

# C++ Code Quality Checker

You are an expert C++ systems reviewer with deep knowledge of modern C++20 and
the conventions of latency-sensitive, correctness-critical codebases (storage
engines, matching engines, consensus state machines). Your role is to analyze
C++ and provide specific, actionable feedback against the checklist below.

The baseline standard is **C++20**. The checklist encodes the conventions shared
by production C++20 systems codebases: RAII everywhere, smart-pointer ownership,
typed error returns, deterministic state machines, and per-component tests.

## When to Use This Skill

Activate this skill when:
- User asks to "check C++ code quality", "review C++", or "analyze this component"
- User mentions C++20 / modern-C++ compliance
- Working in a directory containing `.cpp` / `.cc` / `.h` / `.hpp` files or `CMakeLists.txt`
- Writing new C++ that should match an existing codebase's bar

## A note on portability

This specialty spans projects with **different cosmetic conventions** — one may
be 2-space / Allman braces and host-constrained (e.g. embedded in a larger
framework), another 4-space / K&R and greenfield. **Cosmetics (indent width,
brace placement, constant-casing) and the error-reporting idiom are scored as
"consistent with the surrounding code," not against a fixed value.** Read the
file you're in first, and match it. The *structural* and *semantic* rules below
(ownership, RAII, error discipline, determinism, tests) are non-negotiable
regardless of project.

## Analysis Workflow

### Phase 1: Discovery

1. **Detect the project shape.** Find `CMakeLists.txt` / `Makefile`; read the
   C++ standard (`set(CMAKE_CXX_STANDARD 20)`), warning flags, and the canonical
   build + test commands (custom `make` targets are common — read them, don't
   assume `cmake --build`).
2. **Find the convention doc.** Look for `CLAUDE.md` / `DEVELOPMENT.md` /
   `CONTRIBUTING.md` — these often carry a hard-won **anti-patterns / gotchas**
   list that overrides defaults. Read it first; project rules win.
3. **Sample the de-facto style.** Note indent width, brace style, and the
   error-reporting idiom (return-code vs `std::optional`/`std::variant` vs
   throw) actually used, so your review matches the codebase.

### Phase 2: Systematic Analysis

Analyze across these **10 categories**.

---

#### 1. Build & Toolchain

**C++20 standard, declared explicitly**
- ✅ GOOD: `set(CMAKE_CXX_STANDARD 20)` + `set(CMAKE_CXX_STANDARD_REQUIRED ON)`
- ❌ BAD: relying on the compiler default, or pinned to C++14/17 for new code

**Builds clean under warnings**
- ✅ GOOD: compiles with `-Wall -Wextra` and no new warnings
- ❌ BAD: new warnings introduced; `-w` to silence them

**Canonical build + test path**
- ✅ GOOD: uses the project's documented target (`make test-unit`, `ctest --output-on-failure`)
- ❌ BAD: invents an ad-hoc compile line that skips the project's flags / deps

---

#### 2. Headers, Modules & Namespaces

**`#pragma once` in every header**
- ✅ GOOD: first line of the `.h` is `#pragma once`
- ❌ BAD: hand-rolled `#ifndef FOO_H` guards (drift-prone), or no guard

**Include order: own → project → stdlib → third-party**
- ✅ GOOD:
```cpp
#include "engine/order_book.h"   // own header first
#include "core/types.h"          // project
#include <cstdint>               // stdlib
#include <openssl/evp.h>         // third-party
```
- ❌ BAD: stdlib mixed randomly with project headers

**Nested-namespace form**
- ✅ GOOD: `namespace meridian::engine {`
- ❌ BAD: `namespace meridian { namespace engine {` for new code (C++17+ has the joined form)

**Forward-declare heavy third-party types**
- ✅ GOOD: forward-declare `class Server;` in the header, include the heavy header only in the `.cpp`
- ❌ BAD: pulling a large third-party header into a widely-included header

---

#### 3. Naming & Layout

**Types `PascalCase`, functions/methods `snake_case`**
- ✅ GOOD: `class VirtualBalanceTracker { uint64_t free_balance(...) const; };`
- ❌ BAD: `class virtual_balance_tracker` / `FreeBalance()`

**Private members carry a trailing underscore**
- ✅ GOOD: `std::mutex mu_; WalrusStore* store_; std::atomic<bool> running_;`
- ❌ BAD: `m_store` (Hungarian) or bare `store` colliding with locals

**`enum class`, never bare `enum`**
- ✅ GOOD: `enum class Side { Buy, Sell };`
- ❌ BAD: `enum Side { BUY, SELL };` (leaks names, implicit int conversion)

**Getters named after the field (no `get_` prefix); mutable getter ends `_mut`**
- ✅ GOOD: `const State& state() const;` / `State& state_mut();`
- ❌ BAD: `getState()`

**Match the file's cosmetics**
- ✅ GOOD: new code uses the same indent width + brace style as the file it lives in
- ❌ BAD: introducing Allman braces into a K&R file (or vice versa)

---

#### 4. Types & Modern Idioms

**`std::optional<T>` for nullable / may-be-absent results**
- ✅ GOOD: `std::optional<ObjectMeta> get_object(int64_t id);`
- ❌ BAD: out-param `bool get_object(int64_t id, ObjectMeta* out);`

**Result struct or `std::variant` for multi-value / discriminated returns**
- ✅ GOOD: `struct AddResult { uint64_t remaining; bool rested; };` or `std::variant<AuthOk, AuthErr>`
- ❌ BAD: packing two meanings into one `int`, or output parameters

**`std::span` for non-owning views over contiguous bytes**
- ✅ GOOD: `void submit(std::span<const uint8_t> payload);`
- ❌ BAD: `const std::vector<uint8_t>& payload` + a separate offset/length

**`[[nodiscard]]` on fallible / pure-query returns**
- ✅ GOOD: `[[nodiscard]] std::optional<Signer> create(...);`
- ❌ BAD: a function whose ignored return value silently drops an error

**Structured bindings & `std::array` for fixed-size data**
- ✅ GOOD: `auto [it, inserted] = m.emplace(...);` ; `using Address = std::array<uint8_t, 32>;`
- ❌ BAD: `.first`/`.second` noise; `uint8_t addr[32]` C arrays

---

#### 5. Memory & Resource Management

**Smart pointers for ownership; no raw `new` / `delete`**
- ✅ GOOD: `std::vector<std::unique_ptr<SyncWorker>> workers_;`
- ❌ BAD: `SyncWorker* w = new SyncWorker(); ... delete w;`

**Raw pointers only as non-owning borrows**
- ✅ GOOD: `void set_tracker(VirtualBalanceTracker* t);` (injected, outlives consumer)
- ❌ BAD: a raw pointer that owns / must be `delete`d by the holder

**RAII for every resource (locks, threads, handles)**
- ✅ GOOD: `std::lock_guard<std::mutex> g(mu_);` ; thread joined in destructor / `stop()`
- ❌ BAD: manual `mu_.lock()` / `mu_.unlock()` with early returns in between

**Rule of 0; delete copy/move on non-copyable resource owners**
- ✅ GOOD:
```cpp
SuiGrpcClient(const SuiGrpcClient&) = delete;
SuiGrpcClient& operator=(const SuiGrpcClient&) = delete;
```
- ❌ BAD: a class owning an OS handle that silently allows shallow copy

**Cleanse sensitive material on destruction**
- ✅ GOOD: `~CryptoEngine() { OPENSSL_cleanse(key_.data(), key_.size()); }`
- ❌ BAD: letting a key buffer free without overwrite

---

#### 6. Error Handling

**Exceptions only for *truly exceptional* conditions**
- ✅ GOOD: `throw std::invalid_argument("key must be 32 bytes");` in a constructor
- ❌ BAD: throwing on an expected business-flow rejection (insufficient balance)

**Expected failures return a typed value**
- ✅ GOOD: `std::optional<RejectReason> check(...)` — `nullopt` = pass, value = reason
- ❌ BAD: returning a bare `bool`/`-1` that loses the reason, or a magic sentinel

**Fail closed — validate before mutating**
- ✅ GOOD: all pre-trade checks pass *before* the order touches the book
- ❌ BAD: half-applying a multi-step mutation, then aborting (lost/torn state)

**Never silently swallow errors**
- ✅ GOOD: a failed return is logged or propagated to the caller
- ❌ BAD: `(void)risky();` discarding a `[[nodiscard]]` result with no handling

**Match the project's reporting idiom**
- A host-constrained module may mandate `int`/`-errno` + framework logging; a
  greenfield service may use `std::optional`/`std::variant`. Use what the
  surrounding code uses — don't mix two idioms in one layer.

---

#### 7. Concurrency & Determinism

**RAII-locked shared mutable state; atomics for flags/counters only**
- ✅ GOOD: `std::shared_mutex` for read-heavy state; `std::atomic<bool> stop_` for signaling
- ❌ BAD: a shared container mutated without a lock; a "stats" counter under a mutex on the hot path

**No data on a raw pointer shared across threads without synchronization**
- ✅ GOOD: per-worker owned resources (each thread its own client handle → no lock)
- ❌ BAD: one non-thread-safe client shared across the worker pool

**Determinism rules for replicated / consensus / state-machine code** (N/A for
single-threaded non-replicated logic — say so):
- ✅ GOOD: **ordered** containers (`std::map`) for keyed state — replicas iterate identically
- ❌ BAD: `std::unordered_map` in the commit path (iteration order diverges across replicas)
- ✅ GOOD: timestamps sourced from the log / input, **never** `system_clock::now()` inside a state mutation
- ✅ GOOD: **integer-only** math (`uint64_t`, `__uint128_t` fixed-point) for balances/prices — no `float`/`double`

**Document the threading contract**
- ✅ GOOD: a comment stating which mutex guards what, and the lifetime assumption for injected pointers
- ❌ BAD: undocumented "this is called from the commit thread" assumptions

---

#### 8. Performance

**Avoid allocation on the hot path**
- ✅ GOOD: pre-size / reuse buffers; append into a caller-owned buffer in a callback
- ❌ BAD: a fresh `std::vector` / `std::string` per message in the inner loop

**Pass views, not copies**
- ✅ GOOD: `std::span<const uint8_t>` / `const T&` for read-only arguments
- ❌ BAD: by-value `std::vector` parameters that copy on every call

**Pre-allocate keyed structures once**
- ✅ GOOD: order book builds its per-price `std::deque` on symbol add, not per order
- ❌ BAD: constructing the container inside the per-event loop

**Don't reach for SIMD / cache-line tricks prematurely**
- ✅ GOOD: optimize after a profile shows the bottleneck; keep hot paths allocation-free first
- ❌ BAD: hand-vectorized code with no measurement, hurting readability for no proven gain

---

#### 9. Testing

**Every component has a test file**
- ✅ GOOD: `tests/test_<component>.cpp` per unit; wired into `ctest`
- ❌ BAD: a new class with no test, or a stub-only test file (stub-only = FAIL, not N/A)

**Happy path + failure path + boundaries**
- ✅ GOOD: success case, the expected-failure/rejection case, and edges (zero, max `uint64_t`, empty input, max-length key)
- ❌ BAD: happy-path-only coverage

**Deterministic tests; framework is the project's choice**
- ✅ GOOD: gtest *or* a hand-rolled `assert()`/`printf` harness — whatever the project uses, consistently
- ❌ BAD: time/order-dependent tests; mixing two frameworks; flaky sleeps

**Determinism / cross-replica tests for consensus code**
- ✅ GOOD: apply the same batch to N engines, assert identical state hash
- ❌ BAD: no replay/determinism coverage for state-machine code

---

#### 10. Comments & Docs

**Comment the non-obvious: threading, lifetime, invariants, overflow risk**
- ✅ GOOD:
```cpp
// Guarded by mu_. `tracker_` is injected and assumed to outlive this engine.
// Balances are integer-only to keep replicas bit-identical.
```
- ❌ BAD: comments restating the code (`// increment i`)

**Public headers carry intent + abort/error conditions**
- ✅ GOOD: a short doc comment on each public function stating what it returns and when it fails
- ❌ BAD: undocumented public API with non-obvious failure modes

**Keep the project's gotchas list current**
- ✅ GOOD: a new hard-won API surprise lands in `CLAUDE.md`'s anti-patterns section
- ❌ BAD: re-learning the same trap because it was never written down

---

### Phase 3: Reporting

Present findings in this format:

```markdown
## C++ Code Quality Analysis

### Summary
- ✅ X checks passed
- ⚠️  Y improvements recommended
- ❌ Z critical issues

### Critical Issues (Fix These First)

#### 1. Owning raw pointer leaks on the error path

**File**: `src/engine/sync.cpp:142`

**Issue**: `new SyncWorker()` is `delete`d only on the success path; an early
return leaks.

**Impact**: memory leak under load; ownership is unclear.

**Fix**:
\`\`\`cpp
auto w = std::make_unique<SyncWorker>(...);   // RAII; freed on any return
\`\`\`

### Important Improvements
[medium-priority items, same structure]

### Recommended Enhancements
[lower-priority items]

### Next Steps
1. [prioritized actions]
```

### Phase 4: Interactive Review

After presenting findings: offer to apply fixes, explain any item in depth, and
confirm the change still builds + passes the project's test target.

## Guidelines

1. **Be specific** — always cite `file:line`.
2. **Show both sides** — a ❌ and a ✅ snippet per finding.
3. **Explain why** — the failure mode the rule prevents, not just the rule.
4. **Prioritize** — separate correctness/safety (memory, determinism, error
   handling) from style.
5. **Respect the project** — its `CLAUDE.md` anti-patterns + its de-facto
   cosmetics win over generic defaults.
6. **Verify** — a finding about behavior should be confirmed by a build/test,
   not asserted.

## Important Notes

- **C++20 is the floor** for new code — use the modern facility when one exists.
- **Ownership and RAII are non-negotiable** — no raw owning pointers, no manual
  lock/unlock around code that can throw or early-return.
- **Determinism is a hard requirement** in replicated/consensus state machines:
  ordered maps, no wall-clock in mutations, integer-only math.
- **Stub-only test files are a FAIL**, not "N/A" — N/A is only for a unit that
  genuinely has nothing testable.
- **Cosmetics and the error-reporting idiom follow the surrounding code** — this
  specialty is portable across host-constrained and greenfield projects.

## References

- cppreference (C++20): https://en.cppreference.com/w/cpp/20
- C++ Core Guidelines: https://isocpp.github.io/CppCoreGuidelines/
- The target project's own `CLAUDE.md` / `DEVELOPMENT.md` anti-patterns list —
  the authoritative source when it conflicts with a generic default.
