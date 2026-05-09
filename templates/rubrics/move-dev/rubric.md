---
name: move-dev
version: v1
---

# Rubric: move-dev (v1)

Sources: Move book code-quality checklist (https://move-book.com/guides/code-quality-checklist/),
Sui examples (https://github.com/MystenLabs/sui-examples), Mysten Move conventions
(internal `sui-dev-skills/sui-move/SKILL.md`), Quorum sweeping-sui review history.

The `evaluator` agent (Phase 8 Track 1) reads this file, walks each item, and
emits per-item pass/fail in the EVALUATION block (Phase 8 Track 3). Categories
are documentation; per-item `(W)` weights drive scoring.

## Compilation and tests (weight 20)
- [ ] (6) Package builds with `sui move build` cleanly
- [ ] (6) All package tests pass under `sui move test`
- [ ] (4) No new compiler warnings introduced
- [ ] (4) Move.toml declares `edition = "2024.beta"` or newer

## Move 2024 idioms (weight 20)
- [ ] (3) Module uses single-line `module pkg::name;` form, not legacy braces
- [ ] (3) All structs declared `public` with abilities after fields
- [ ] (3) Mutable bindings declared `let mut`, not bare `let`
- [ ] (3) Method syntax preferred for first-arg-typed receivers (e.g. `coin.value()`)
- [ ] (2) `vector[...]` literal + index syntax used instead of `vector::empty` + `push_back`
- [ ] (2) Constants follow `EPascalCase` for errors, `ALL_CAPS` otherwise
- [ ] (2) Getters named after the field (no `get_` prefix); mutable getters end in `_mut`
- [ ] (2) `entry` functions stand alone — no `public entry` combination

## Capabilities (weight 15)
- [ ] (4) Capability structs suffixed with `Cap` and held by `key, store`
- [ ] (4) Privileged functions take a `&Cap` argument (not a `ctx.sender()` check)
- [ ] (4) Cross-vault destinations re-validate against the registry allowlist inside the helper, not at the wrapper boundary
- [ ] (3) Capability objects never embedded inside shared objects without explicit revocation design

## Aborts and errors (weight 15)
- [ ] (4) Error constants named `EPascalCase`, declared `const`, with stable numeric values
- [ ] (4) Public entry points fail closed — invalid input aborts before any state mutation
- [ ] (3) `assert!` calls without abort codes only used for line-number-derived clever errors, not in tests asserted by code
- [ ] (4) No silent `Option::destroy_some` or `unwrap`-style destructuring on caller-supplied data — wrap in `do!` / `destroy_or!`

## Transfer semantics (weight 10)
- [ ] (3) `transfer::transfer` / `share_object` / `freeze_object` only called inside the module that defines the type
- [ ] (3) Cross-module transfers go through `transfer::public_transfer` (requires `key + store`)
- [ ] (2) Hot-potato structs (no abilities) destructured at the call site, never returned to caller as drop fodder
- [ ] (2) Pure logic functions return `Coin<T>` / `Balance<T>` to the caller — no inline `transfer` inside swap / sweep core

## Test coverage (weight 10)
- [ ] (3) Happy-path test exists for every `public` / `entry` function
- [ ] (3) Authorization-failure test exists for every capability-gated function (uses `#[expected_failure]`)
- [ ] (2) Boundary inputs covered (zero, max u64, empty vector, 32-byte key length)
- [ ] (2) Tests use `assert_eq!` for value comparisons; `assert!` reserved for booleans

## Comments and docs (weight 10)
- [ ] (3) Module-level `///` doc comment states purpose and any cross-module invariants
- [ ] (3) Every `public` / `entry` function has a `///` doc comment describing intent + abort conditions
- [ ] (2) Non-obvious math / underflow risk / off-chain caller assumptions called out with inline `//` comments
- [ ] (2) Events documented at the struct level — emitter, payload meaning, indexer relevance
