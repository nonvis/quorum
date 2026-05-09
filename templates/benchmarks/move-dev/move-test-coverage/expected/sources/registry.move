/// Simple name registry. Anyone can register an unused name (≤ 32 bytes);
/// transfer or release require holding the matching `NameCap`.
module registry::registry;

use std::string::{Self, String};
use sui::table::{Self, Table};

const ENameTooLong: u64 = 1;
const ENameTaken: u64 = 2;
const EUnauthorized: u64 = 3;
const ENameNotFound: u64 = 4;

const MAX_NAME_LEN: u64 = 32;

/// Shared registry mapping name → owner address.
public struct Registry has key {
    id: UID,
    names: Table<String, address>,
}

/// Capability proving the holder owns `name`.
public struct NameCap has key, store {
    id: UID,
    name: String,
}

fun init(ctx: &mut TxContext) {
    let r = Registry {
        id: object::new(ctx),
        names: table::new(ctx),
    };
    transfer::share_object(r);
}

#[test_only]
public fun init_for_testing(ctx: &mut TxContext) { init(ctx) }

/// Register a name. Aborts if name > 32 bytes or already taken.
public entry fun register(r: &mut Registry, name: vector<u8>, ctx: &mut TxContext) {
    let s = string::utf8(name);
    assert!(string::length(&s) <= MAX_NAME_LEN, ENameTooLong);
    assert!(!table::contains(&r.names, s), ENameTaken);

    let owner = ctx.sender();
    table::add(&mut r.names, s, owner);

    let cap = NameCap {
        id: object::new(ctx),
        name: s,
    };
    transfer::public_transfer(cap, owner);
}

/// Transfer a name to a new owner. Capability-gated.
public entry fun transfer_name(
    r: &mut Registry,
    cap: NameCap,
    new_owner: address,
    _ctx: &mut TxContext,
) {
    assert!(table::contains(&r.names, cap.name), ENameNotFound);
    let current_owner = table::borrow_mut(&mut r.names, cap.name);
    assert!(*current_owner == cap.name_owner_address(r), EUnauthorized);

    *current_owner = new_owner;

    let NameCap { id, name } = cap;
    let new_cap = NameCap { id, name };
    transfer::public_transfer(new_cap, new_owner);
}

/// Release a name back to the pool. Burns the cap.
public entry fun release(r: &mut Registry, cap: NameCap) {
    assert!(table::contains(&r.names, cap.name), ENameNotFound);
    let _owner = table::remove(&mut r.names, cap.name);

    let NameCap { id, name: _ } = cap;
    object::delete(id);
}

/// Look up the owner of a name. Aborts if name isn't registered.
public fun owner_of(r: &Registry, name: vector<u8>): address {
    let s = string::utf8(name);
    assert!(table::contains(&r.names, s), ENameNotFound);
    *table::borrow(&r.names, s)
}

/// Returns true if `name` is currently registered.
public fun is_registered(r: &Registry, name: vector<u8>): bool {
    table::contains(&r.names, string::utf8(name))
}

/// Helper: look up the registered owner for the name on this cap.
fun name_owner_address(cap: &NameCap, r: &Registry): address {
    *table::borrow(&r.names, cap.name)
}
