---
name: sui-move
description: Sui Move 2024 smart contract development. Use when writing, reviewing, or debugging Sui Move code, Move.toml configuration, or Sui object model patterns.
---

# Sui Move Development Skill

You are writing Sui Move smart contracts. Follow these rules precisely. Sui Move is **not** Aptos Move and is **not** Rust — do not apply patterns from those languages.

---

## 1. Package Setup

Always use Move 2024 edition. Every new package's `Move.toml` must include:

```toml
[package]
name = "my_package"
edition = "2024.beta"
```

**Implicit framework dependencies (Sui 1.45+)** — do not list `Sui`, `MoveStdlib`, `Bridge`, or `SuiSystem` in `[dependencies]`. They are implicit:

```toml
# ✅ Sui 1.45+
[dependencies]
# no framework entries needed

# ❌ Outdated
[dependencies]
Sui = { git = "...", subdir = "crates/sui-framework/packages/sui-framework", rev = "..." }
```

**Named addresses** — prefix named addresses with your project name to avoid conflicts:

```toml
# ✅
[addresses]
my_protocol_amm = "0x0"

# ❌ Too generic — prone to collisions
[addresses]
amm = "0x0"
```

Run `sui move build` after any significant change to verify the code compiles before proceeding.

---

## 2. Module Layout (Move 2024)

Use the new single-line module declaration without braces:

```move
// ✅ Move 2024
module my_package::my_module;

// ❌ Legacy — do not use
module my_package::my_module {
    ...
}
```

Standard section order within a module:
1. `use` imports
2. Constants (`const`)
3. Structs / Enums
4. `fun init` (if needed)
5. Public functions
6. `public(package)` functions
7. Private functions
8. Test module (`#[test_only]`)

Use `=== Section Title ===` comments to delimit sections for readability.

### `use` import rules

Don't use a lone `{Self}` — just import the module directly:

```move
// ✅
use my_package::my_module;

// ❌ Redundant braces
use my_package::my_module::{Self};
```

When importing both the module and members, group them with `Self`:

```move
// ✅
use sui::coin::{Self, Coin};

// ❌ Separate imports
use sui::coin;
use sui::coin::Coin;
```

---

## 3. Structs

All structs must be declared `public`. Ability declarations go **after** the fields:

```move
// ✅ Move 2024
public struct Pool has key {
    id: UID,
    balance_x: Balance<SUI>,
    balance_y: Balance<USDC>,
}

public struct PoolCap has key, store {
    id: UID,
    pool_id: ID,
}

// ❌ Legacy — no public keyword
struct Pool has key {
    id: UID,
}
```

**Object rule**: Any struct with the `key` ability **must** have `id: UID` as its first field. Use `object::new(ctx)` to create UIDs — never reuse or fabricate them.

### Naming conventions

**Capabilities** must be suffixed with `Cap`:

```move
// ✅
public struct AdminCap has key, store { id: UID }

// ❌ Unclear it's a capability
public struct Admin has key, store { id: UID }
```

**No `Potato` suffix** — a struct's lack of abilities already communicates it's a hot potato:

```move
// ✅
public struct Promise {}

// ❌
public struct PromisePotato {}
```

**Events named in past tense** — they describe something that already happened:

```move
// ✅
public struct LiquidityAdded has copy, drop { ... }
public struct FeesCollected has copy, drop { ... }

// ❌
public struct AddLiquidity has copy, drop { ... }
public struct CollectFees has copy, drop { ... }
```

**Dynamic field keys** — use positional structs (no named fields):

```move
// ✅
public struct BalanceKey() has copy, drop, store;

// ⚠️ Acceptable but not canonical
public struct BalanceKey has copy, drop, store {}
```

### Constants naming

Error constants use `EPascalCase`. All other constants use `ALL_CAPS`:

```move
// ✅
const ENotAuthorized: u64 = 0;
const MAX_FEE_BPS: u64 = 10_000;

// ❌
const NOT_AUTHORIZED: u64 = 0;   // error should be EPascalCase
const MaxFeeBps: u64 = 10_000;   // non-error should be ALL_CAPS
```

---

## 4. Object Abilities Cheat Sheet

| Ability | Meaning in Sui |
|---------|---------------|
| `key` | Struct is an on-chain object; requires `id: UID` as first field |
| `store` | Can be embedded inside other objects; enables `public_transfer`, `public_share_object`, `public_freeze_object` |
| `copy` | Value can be duplicated (not valid on objects with `key`) |
| `drop` | Value can be silently discarded |

**Object ownership model:**

```move
// Transfer to an address (owned object)
transfer::transfer(obj, recipient);             // key only — module-restricted
transfer::public_transfer(obj, recipient);      // key + store — usable anywhere

// Share (accessible by anyone, goes through consensus)
transfer::share_object(obj);                    // key only — module-restricted
transfer::public_share_object(obj);             // key + store

// Freeze (immutable forever)
transfer::freeze_object(obj);                   // key only — module-restricted
transfer::public_freeze_object(obj);            // key + store
```

Only call `transfer`, `share_object`, and `freeze_object` (the non-`public_` variants) inside the module that **defines** that object's type.

**Never** construct an object struct literal outside of its defining module.

---

## 5. Mutability — `mut` is Required

All variables that are reassigned or mutably borrowed must be declared `let mut`:

```move
// ✅ Move 2024
let mut pool = Pool { id: object::new(ctx), ... };
let mut balance = balance::zero<SUI>();

// ❌ Legacy
let pool = Pool { id: object::new(ctx), ... };
```

Function parameters that are mutably borrowed must also be `mut`:

```move
public fun deposit(mut pool: Pool, coin: Coin<SUI>): Pool { ... }
```

---

## 6. Visibility

| Keyword | Scope |
|---------|-------|
| `public` | Callable from any module |
| `public(package)` | Callable only within the same package |
| *(none)* | Private — callable only within the same module |

`public(friend)` and `friend` declarations are **deprecated**. Use `public(package)` instead.

```move
// ✅
public(package) fun internal_logic(pool: &mut Pool) { ... }

// ❌ Deprecated
friend my_package::other_module;
public(friend) fun internal_logic(pool: &mut Pool) { ... }
```

**Never use `public entry`** — use one or the other. `public` functions are composable in PTBs and return values; `entry` functions are transaction endpoints only and cannot return values:

```move
// ✅ Composable — can be chained in PTBs
public fun mint(ctx: &mut TxContext): NFT { ... }

// ✅ Transaction endpoint — no return value needed
entry fun mint_and_transfer(recipient: address, ctx: &mut TxContext) { ... }

// ❌ Redundant combination — avoid
public entry fun mint(ctx: &mut TxContext): NFT { ... }
```

### Function parameter ordering

Always order parameters: mutable objects → immutable objects → capabilities → primitive types → `&Clock` → `&mut TxContext`:

```move
// ✅
public fun call(
    app: &mut App,
    config: &Config,
    cap: &AdminCap,
    amount: u64,
    is_active: bool,
    clock: &Clock,
    ctx: &mut TxContext,
) { }

// ❌ Wrong order
public fun call(
    amount: u64,
    app: &mut App,
    cap: &AdminCap,
    config: &Config,
    ctx: &mut TxContext,
) { }
```

### Getter naming

Name getters after the field. Do not use a `get_` prefix:

```move
// ✅
public fun fee_bps(pool: &Pool): u64 { pool.fee_bps }
public fun fee_bps_mut(pool: &mut Pool): &mut u64 { &mut pool.fee_bps }

// ❌
public fun get_fee_bps(pool: &Pool): u64 { pool.fee_bps }
```

---

## 7. Method Syntax

In Move 2024, functions whose first argument matches a type are automatically callable as methods:

```move
// Given:
public fun value(coin: &Coin<SUI>): u64 { coin.value() }

// Both are valid, prefer the method form:
let v = coin.value();       // ✅ method syntax — prefer this
let v = coin::value(&coin); // ✅ also valid, but more verbose
```

Use method syntax wherever it improves readability. Declare `use fun` aliases for functions defined outside the owning module:

```move
use fun my_module::pool_value as Pool.value;
```

---

## 8. Enums (Move 2024)

Use enums for types with multiple variants. Enums **cannot** have the `key` ability (they cannot be top-level objects), but they can be stored inside objects:

```move
public enum OrderStatus has copy, drop, store {
    Pending,
    Filled { amount: u64 },
    Cancelled,
}
```

Pattern match with `match`:

```move
match (order.status) {
    OrderStatus::Pending => { ... },
    OrderStatus::Filled { amount } => { /* use amount */ },
    OrderStatus::Cancelled => { ... },
}
```

---

## 9. Macros

Use macro functions for higher-order patterns instead of manual loops.

### Vector macros

```move
// Do something N times
32u8.do!(|_| do_action());

// Build a new vector from an index range
let v = vector::tabulate!(32, |i| i);

// Iterate by immutable reference
vec.do_ref!(|e| process(e));

// Iterate by mutable reference
vec.do_mut!(|e| *e = *e + 1);

// Consume vector, calling a function on each element
vec.destroy!(|e| handle(e));

// Fold into a single value
let sum = vec.fold!(0u64, |acc, x| acc + x);

// Filter (requires T: drop)
let big = vec.filter!(|x| *x > 100);
```

All of these replace verbose manual `while` loops. Use them whenever you iterate over a vector.

### Option macros

```move
// Execute a function if Some, then drop
opt.do!(|value| process(value));

// Unwrap with a default (or abort)
let value = opt.destroy_or!(default);
let value = opt.destroy_or!(abort ECannotBeEmpty);
```

These replace verbose `if (opt.is_some())` / `destroy_some()` patterns:

```move
// ❌ Verbose
if (opt.is_some()) {
    let inner = opt.destroy_some();
    process(inner);
};

// ✅
opt.do!(|inner| process(inner));
```

---

## 10. Common Standard Library Patterns

```move
// Strings — use method syntax, don't import utf8
let s: String = b"hello".to_string();
let ascii: ascii::String = b"hello".to_ascii_string();

// Coin and Balance
use sui::coin::{Self, Coin};
use sui::balance::{Self, Balance};

let balance: Balance<SUI> = coin.into_balance();
let coin: Coin<SUI> = balance.into_coin(ctx);  // ✅ method syntax
let amount: u64 = coin.value();

// Split a payment
let exact = payment.split(amount, ctx);        // ✅
let exact = payment.balance_mut().split(amount); // ✅ avoids ctx

// Consuming values without `drop` — the @0x0 burn pattern
//
// Sui Move's linear type system requires every non-`drop` value to be
// explicitly consumed. The `_` prefix only suppresses warnings for values
// that *do* have `drop` — it won't help for Balance<T>, Coin<T>, or your
// own structs that lack `drop`.
//
// To permanently destroy any `key + store` object, transfer it to @0x0
// (an address no one controls, equivalent to Solidity's address(0)):
transfer::public_transfer(my_obj, @0x0);       // ✅ permanent burn
//
// Balance<T> has neither `drop` nor `key`, so it cannot be transferred
// directly, and `balance::destroy_zero` only works on empty balances.
// Wrap it in a Coin first:
//
//   let _locked = supply.increase_supply(MINIMUM_LIQUIDITY); // ❌ compile error
//
let locked = supply.increase_supply(MINIMUM_LIQUIDITY).into_coin(ctx);
transfer::public_transfer(locked, @0x0);       // ✅ burns the minimum liquidity
//
// Hot potatoes (structs with no abilities at all) cannot use this pattern —
// they must be destructured and each field consumed individually.

// Option
let opt: Option<u64> = option::some(42);
let val = opt.destroy_or!(default_value);      // ✅ macro form
let val = opt.borrow();

// Address and IDs
let id: ID = object::id(&my_obj);
let addr: address = id.to_address();

// UID deletion
id.delete();                                   // ✅
// object::delete(id);                         // ❌ verbose

// TxContext sender
ctx.sender()                                   // ✅
// tx_context::sender(ctx)                     // ❌ verbose

// Vector literals and index syntax
let mut v = vector[1, 2, 3];                   // ✅ literal
let first = v[0];                              // ✅ index syntax
assert!(v.length() == 3);                      // ✅ method syntax
// let mut v = vector::empty();               // ❌ verbose
// vector::push_back(&mut v, 1);              // ❌ verbose

// Struct unpack — use .. to ignore fields you don't need
let MyStruct { id, .. } = value;               // ✅
// let MyStruct { id, field_a: _, field_b: _ } = value; // ❌ verbose
```

---

## 11. Events

Emit events for all state-changing operations that clients need to observe:

```move
use sui::event;

public struct LiquidityAdded has copy, drop {
    pool_id: ID,
    amount_x: u64,
    amount_y: u64,
    lp_minted: u64,
}

// Inside function:
event::emit(LiquidityAdded {
    pool_id: object::id(pool),
    amount_x,
    amount_y,
    lp_minted,
});
```

---

## 12. Error Handling

Error constants use `EPascalCase` and `u64` values:

```move
const EInsufficientLiquidity: u64 = 0;
const EZeroAmount: u64 = 1;

assert!(amount > 0, EZeroAmount);
```

### Clever errors (Move 2024)

Annotating a constant with `#[error]` allows it to carry a human-readable message. The value can be any valid constant type — `vector<u8>` is most common for string messages:

```move
#[error]
const EInsufficientLiquidity: vector<u8> = b"Insufficient liquidity in pool";

assert!(reserves > 0, EInsufficientLiquidity);
abort EInsufficientLiquidity
```

At runtime, the Sui CLI and GraphQL server automatically decode these into a readable message:
```
Error from '0x2::amm::swap' (line 42), abort 'EInsufficientLiquidity': "Insufficient liquidity in pool"
```

**Gotcha**: clever error abort codes encode the source line number, so their `u64` value can change if the file is reformatted or lines shift. Don't hardcode clever error abort codes in tests or off-chain tooling — match by constant name instead.

**`assert!` without an abort code** is also valid and auto-derives a clever abort code from the source line:

```move
// ✅ Valid — line number is embedded automatically
assert!(amount > 0);
```

This is fine for internal invariants where the line number alone is enough context.

### Fail-closed semantics

Every `public` / `entry` function must validate inputs **before** any state mutation. If validation aborts after a partial write, the transaction reverts cleanly because Sui Move is transactional — but the bug is in *which* aborts your tests are observing. Order checks first, mutations second:

```move
// ✅ Fail closed — aborts before any state changes
public fun sweep_to_trusted<T>(
    vault: &mut DepositVault,
    registry: &Registry,
    recipient: address,
    receiving: Receiving<Coin<T>>,
) {
    assert!(registry.is_trusted(recipient), ERecipientNotTrusted); // check first
    let coin = transfer::public_receive(&mut vault.id, receiving); // then mutate
    // ...
}
```

For `Option`, prefer `do!` and `destroy_or!` — never silently `destroy_some` user-supplied data:

```move
// ✅ explicit failure mode
let amount = supplied_amount.destroy_or!(abort EAmountRequired);

// ❌ panics opaquely if None
let amount = supplied_amount.destroy_some();
```

---

## 13. One-Time Witness (OTW) Pattern

Use the OTW pattern for modules that need a unique, uncopyable proof-of-publication (e.g., coin types, publisher objects):

```move
public struct MY_MODULE has drop {}

fun init(otw: MY_MODULE, ctx: &mut TxContext) {
    // The OTW name must exactly match the module name in ALL_CAPS
    let publisher = package::claim(otw, ctx);
    transfer::public_transfer(publisher, ctx.sender());
}
```

---

## 14. Capability Pattern

Use capability objects to gate privileged functions instead of checking `ctx.sender()`. This is more composable and testable — the capability can be held by a contract, not just a wallet:

```move
// ✅ Capability-gated
public struct AdminCap has key, store { id: UID }

public fun set_fee(_: &AdminCap, pool: &mut Pool, new_fee: u64) {
    pool.fee_bps = new_fee;
}

// ❌ Sender check — not composable with other contracts
public fun set_fee(pool: &mut Pool, ctx: &TxContext) {
    assert!(ctx.sender() == pool.admin, ENotAdmin);
}
```

Note the parameter order: the object (`pool`) comes before the primitive (`new_fee`), and `_: &AdminCap` follows the objects-then-capabilities ordering from section 6.

### Validating untrusted destinations inside the helper

When a privileged function accepts an arbitrary `address` from the caller (e.g. a sweep function routing funds to an operator-supplied `recipient`), the trust check **must live inside the helper that touches state**, not in each wrapper that calls it:

```move
// ✅ Trust check sits next to the state mutation it gates
public fun sweep_to_trusted<T>(
    vault: &mut DepositVault,
    registry: &Registry,
    _cap: &SweepCap,
    recipient: address,
    /* ... */
) {
    assert!(registry.is_trusted(recipient), ERecipientNotTrusted);
    // ... state mutation
}

// ❌ Wrapper does the check, helper trusts the address
public fun sweep_to_trusted<T>(/* ... */ recipient: address) {
    assert!(registry.is_trusted(recipient), ERecipientNotTrusted);
    sweep_internal(vault, recipient); // helper has no knowledge of the check
}

fun sweep_internal<T>(vault: &mut DepositVault, recipient: address) {
    // Adding a new caller of sweep_internal that forgets the assert!
    // silently bypasses the allowlist.
}
```

A wrapper-only check fails when a future refactor adds another caller to the helper without re-applying the assert.

---

## 15. Pure Functions and Composability

Keep core logic functions **pure** — they take objects by reference/value and return values. Do not call `transfer::transfer` inside core logic functions:

```move
// ✅ Pure — composable with other protocols
public fun swap<X, Y>(
    pool: &mut Pool<X, Y>,
    coin_in: Coin<X>,
    ctx: &mut TxContext,
): Coin<Y> {
    // ... swap logic
}

// ❌ Transfer inside core logic breaks composability
public fun swap<X, Y>(pool: &mut Pool<X, Y>, coin_in: Coin<X>, ctx: &mut TxContext) {
    let coin_out = /* ... */;
    transfer::public_transfer(coin_out, ctx.sender()); // ❌
}
```

Return excess coins even if their value is zero — let the caller decide what to do with them.

---

## 16. Dynamic Fields

Use dynamic fields for extensible storage or when the key set is not known at compile time:

```move
use sui::dynamic_field as df;
use sui::dynamic_object_field as dof;

// Add a dynamic field (value stored inline with parent)
df::add(&mut parent.id, key, value);

// Add a dynamic object field (value is an independent object)
dof::add(&mut parent.id, key, child_obj);

// Access
let val: &MyType = df::borrow(&parent.id, key);
let val: &mut MyType = df::borrow_mut(&mut parent.id, key);

// Remove
let val: MyType = df::remove(&mut parent.id, key);
```

---

## 17. Comments

Use `///` for doc comments. JavaDoc-style `/** */` is not supported in Move:

```move
/// Returns the current fee in basis points.
public fun fee_bps(pool: &Pool): u64 { pool.fee_bps }

// ❌ Not supported
/** Returns the current fee in basis points. */
public fun fee_bps(pool: &Pool): u64 { pool.fee_bps }
```

Use regular `//` comments to explain non-obvious logic, potential edge cases, and TODOs:

```move
// Note: can underflow if reserve is smaller than minimum_liquidity.
// TODO: add assert! guard before production use.
let lp_supply = math::sqrt(reserve_x * reserve_y);
```

### Module / function / event docs

A reviewer should be able to read the doc comments alone and reconstruct the trust model:

```move
// Module-level — purpose, capabilities, cross-module invariants.
module sweeping_sui::sweep;

/// Materialize a `DepositVault` for `(registry, key)`. SweepCap-gated.
/// Emits `VaultClaimed`. Aborts with `EInvalidKeyLength` if `key.length() != 32`.
public fun claim_vault(...) { ... }

/// Emitted by every `sweep_*` function. `from` is the vault address,
/// `to` is the destination, `amount` is the swept value.
public struct FundsSwept<phantom T> has copy, drop { ... }
```

Required content per item:
- **Module header**: one-paragraph purpose, who can call what, any cross-module assumptions (e.g. "off-chain caller must filter empty vaults").
- **Public / entry functions**: intent, abort conditions (which `E*` constants and when), capability requirement, side effects (events emitted).
- **Events**: which function emits it, what each field means, why an indexer cares.

---

## 18. Building and Testing

Always verify code compiles and tests pass using the Sui CLI:

```bash
# Build
sui move build

# Run all tests
sui move test

# Run a specific test by name
sui move test swap_exact_input
```

### Test conventions

**Naming** — do not prefix test functions with `test_`. The `#[test]` attribute already signals intent:

```move
// ✅
#[test] fun create_pool() { }
#[test] fun swap_returns_correct_amount() { }

// ❌
#[test] fun test_create_pool() { }
```

**Merge attributes** — combine `#[test]` and `#[expected_failure]` on one line:

```move
// ✅
#[test, expected_failure(abort_code = EInsufficientLiquidity)]
fun swap_with_zero_input() { ... }

// ❌
#[test]
#[expected_failure(abort_code = EInsufficientLiquidity)]
fun swap_with_zero_input() { ... }
```

**Don't clean up in `expected_failure` tests** — let them abort naturally, don't add `scenario.end()` or other teardown:

```move
// ✅
#[test, expected_failure(abort_code = EInsufficientLiquidity)]
fun swap_with_zero_input() {
    let mut ctx = tx_context::dummy();
    let pool = create_pool(&mut ctx);
    pool.swap(coin::zero(&mut ctx)); // aborts here — done
}

// ❌ — don't clean up after expected failure
#[test, expected_failure(abort_code = EInsufficientLiquidity)]
fun swap_with_zero_input() {
    let mut scenario = test_scenario::begin(@0xA);
    // ... test body ...
    scenario.end(); // unnecessary, misleading
}
```

**Use `tx_context::dummy()` for simple tests** — only reach for `test_scenario` when you genuinely need multi-transaction or multi-sender behaviour:

```move
// ✅ Simple test — no scenario needed
#[test]
fun create_pool() {
    let mut ctx = tx_context::dummy();
    let pool = new_pool(&mut ctx);
    assert_eq!(pool.fee_bps(), 30);
    sui::test_utils::destroy(pool);
}

// ✅ Multi-sender test — scenario is appropriate
#[test]
fun only_admin_can_set_fee() {
    let mut scenario = test_scenario::begin(@admin);
    // ...
    scenario.end();
}
```

**Assertions** — prefer `assert_eq!` over `assert!` for value comparisons (shows both sides on failure), and never pass abort codes to `assert!`:

```move
// ✅
assert_eq!(pool.fee_bps(), 30);
assert!(pool.is_active());

// ❌
assert!(pool.fee_bps() == 30);   // doesn't show the actual value on failure
assert!(pool.is_active(), 0);    // abort code conflicts with app error codes
```

**Destroying objects in tests** — use `sui::test_utils::destroy`, never write custom `destroy_for_testing` functions:

```move
// ✅
use sui::test_utils::destroy;
destroy(pool);

// ❌
pool.destroy_for_testing();
```

### Coverage expectations for capability-gated code

Every capability-gated function needs at least two tests:

1. **Happy path** — caller holds the right cap, function succeeds, state changes as expected.
2. **Authorization failure** — `#[expected_failure]` test that exercises the unauthorized path. For pure cap-gating (the cap is a parameter), the failure is at the type level and is enforced by the compiler. For `is_trusted` / allowlist checks, write an explicit abort-expected test:

```move
#[test, expected_failure(abort_code = sweep::ERecipientNotTrusted)]
fun sweep_to_untrusted_address_aborts() {
    let mut scenario = test_scenario::begin(@admin);
    // ... setup ...
    sweep_coin_to_trusted(&mut vault, &registry, &sweep_cap, key, receiving, @0xBAD);
    // no scenario.end() — let the abort carry the test
}
```

### Boundary inputs

For every public function that takes user input, exercise the edges:
- Numeric: `0`, `u64::MAX`, off-by-one around any internal threshold
- Vector: empty vector, single-element vector, vector at any length-validation boundary (e.g. exactly 32 bytes for `KEY_LEN`)
- Address: `@0x0` (the burn address) when the function takes a destination
- Generic type: at least one test instantiation with a non-`SUI` `Coin<T>` type to catch hard-coded type assumptions

---

## 19. What Sui Move is NOT

| Pattern | Source | Do NOT use in Sui Move |
|---------|--------|------------------------|
| `acquires`, `move_to`, `move_from`, `borrow_global` | Aptos / Core Move | Sui has no global storage |
| `signer` type | Aptos / Core Move | Use `&mut TxContext` and `ctx.sender()` |
| `Script` functions | Aptos | Use `entry` functions instead |
| `public(friend)` | Legacy Sui Move | Use `public(package)` |
| Struct without `public` keyword | Legacy Sui Move | All structs must be `public` in 2024 |
| `let x = ...` for mutable vars | Legacy Sui Move | Use `let mut x = ...` |
| `use` inside function bodies for module-level imports | Style issue | Put `use` at the top of the module |
| `&signer` | Rust / Aptos | Does not exist in Sui Move |

---

## 20. Common Pitfalls

Real failure modes drawn from Quorum review history (`sample/move/sweeping-sui`).
Each pitfall maps to a specific rubric item in `templates/rubrics/move-dev/rubric.md`.

### Forgetting `is_trusted` on cross-vault destinations

**Symptom:** A new `sweep_*_to_address(recipient: address, ...)` entry function added without the `assert!(registry.is_trusted(recipient), ERecipientNotTrusted)` check. Funds can be exfiltrated to an attacker-controlled address.

**Real example:** sweeping-sui's four sweep functions (`sweep_coin_to_omnibus`, `sweep_coin_to_trusted`, `sweep_balance_to_omnibus`, `sweep_balance_to_trusted`) are near-symmetric copies. A DRY refactor that extracts a shared helper must put the trust check inside the helper, not in each wrapper — otherwise adding a fifth caller to the helper silently bypasses the allowlist.

**Maps to rubric:** `capabilities.cross-vault-destinations-re-validate-against-the-registry-allowlist-inside-the-helper-not-at-the-wrapper-boundary`

### Reusing `assert_key_length` only at the public boundary

**Symptom:** Internal `public(package)` paths skip the 32-byte key check on the assumption that public callers already validated. A future caller that bypasses the public path gets undefined `derived_object` behavior.

**Real example:** `registry::assert_key_length` is exposed `public(package)` and called by `sweep::claim_vault` precisely so the invariant lives next to derivation. Don't drop the check in a "fast path" wrapper.

**Maps to rubric:** `aborts-and-errors.public-entry-points-fail-closed-invalid-input-aborts-before-any-state-mutation`

### Capability struct without `Cap` suffix

**Symptom:** `public struct SweepAuth has key, store { id: UID }` — readers can't tell at a glance whether this is a capability, a config object, or a user-facing token. Trust-model audits become harder.

**Real example:** sweeping-sui uses `AdminCap` (cold) and `SweepCap` (hot) — the suffix tells reviewers immediately which functions require which holder.

**Maps to rubric:** `capabilities.capability-structs-suffixed-with-cap-and-held-by-key-store`

### `transfer::transfer` called from outside the defining module

**Symptom:** External module imports `DepositVault` and calls `transfer::transfer(vault, recipient)`. Compiles only because someone made `DepositVault` lack `store`, but now no other module can compose with vaults either.

**Fix:** Inside the defining module use `transfer::transfer` / `transfer::share_object`. Outside the defining module, the type must have `store` and the caller must use `transfer::public_transfer` / `transfer::public_share_object`.

**Maps to rubric:** `transfer-semantics.transfer-transfer-share-object-freeze-object-only-called-inside-the-module-that-defines-the-type`

### Event emitted *before* state mutation completes

**Symptom:** `event::emit(FundsSwept { ... })` runs before the `balance::send_funds` call. If `send_funds` aborts, the transaction reverts cleanly — but during code review the order suggests an "emit-then-mutate" pattern that masks bugs in subsequent refactors (someone moves the emit to after a non-aborting branch and the indexer sees phantom events).

**Real example:** sweeping-sui's `sweep_coin_to_omnibus` pattern is `let amount = coin.value(); ... balance::send_funds(bal, to); event::emit(FundsSwept { ... })` — emit last, after every state mutation has succeeded. Match this ordering.

**Maps to rubric:** `comments-and-docs.events-documented-at-the-struct-level-emitter-payload-meaning-indexer-relevance` and `aborts-and-errors.public-entry-points-fail-closed-invalid-input-aborts-before-any-state-mutation`

### Hot capability stored inside a shared object

**Symptom:** `SweepCap` placed inside the shared `Registry` so anyone can borrow it with `&mut Registry`. The cap is now globally accessible — no off-chain key custody, no revocation path.

**Fix:** Keep `SweepCap` as an owned `key, store` object held by the operator's signing key. Mint via `mint_sweep_cap(&AdminCap, ctx) -> SweepCap` and transfer to the operator. Revocation is off-chain (destroy or relocate the cap object via the holder's keys).

**Maps to rubric:** `capabilities.capability-objects-never-embedded-inside-shared-objects-without-explicit-revocation-design`

### Missing `#[expected_failure]` for the auth-failure path

**Symptom:** Tests cover the happy path of `sweep_coin_to_trusted` but not the "recipient is not on the allowlist" branch. The `assert!(registry.is_trusted(recipient), ERecipientNotTrusted)` line goes uncovered, and a future refactor that flips the boolean direction ships with green tests.

**Fix:** For every capability-gated or allowlist-gated public function, write a `#[test, expected_failure(abort_code = ...)]` test that exercises the unauthorized path.

**Maps to rubric:** `test-coverage.authorization-failure-test-exists-for-every-capability-gated-function-uses-expected-failure`

### Off-chain assumption buried in code, not comments

**Symptom:** sweeping-sui's wallet service must filter empty vaults from the batch PTB or the whole transaction aborts. If that assumption isn't called out in the module header, the next operator team rediscovers it via a production failure.

**Real example:** `sweep.move` opens with a paragraph explaining "Empty vaults must be filtered off-chain before composing a batch sweep PTB." This is the right pattern — surface caller obligations at the module level, not inside individual functions.

**Maps to rubric:** `comments-and-docs.module-level-doc-comment-states-purpose-and-any-cross-module-invariants`