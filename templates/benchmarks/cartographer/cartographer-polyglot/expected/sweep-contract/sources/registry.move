/// Vault registry: records sweep events and the set of known vaults.
module sweep_contract::registry;

use sui::event;

public struct SweepEvent has copy, drop {
    vault: address,
    amount: u64,
}

public fun record_sweep(vault: address, amount: u64) {
    event::emit(SweepEvent { vault, amount });
}
