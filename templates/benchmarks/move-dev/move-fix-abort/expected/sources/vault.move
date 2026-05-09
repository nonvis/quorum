/// Simple coin vault: users deposit SUI, withdraw their balance later.
/// Buggy: re-depositing aborts. Diagnose the abort path and fix it.
module vault::vault;

use sui::balance::{Self, Balance};
use sui::coin::{Self, Coin};
use sui::sui::SUI;
use sui::table::{Self, Table};

/// Shared vault holding per-user balances.
public struct Vault has key {
    id: UID,
    balances: Table<address, Balance<SUI>>,
}

const EZeroDeposit: u64 = 1;
const EInsufficientBalance: u64 = 2;

fun init(ctx: &mut TxContext) {
    let v = Vault {
        id: object::new(ctx),
        balances: table::new(ctx),
    };
    transfer::share_object(v);
}

/// Deposit a Coin<SUI> into the caller's vault balance.
public entry fun deposit(v: &mut Vault, c: Coin<SUI>, ctx: &mut TxContext) {
    let amount = coin::value(&c);
    assert!(amount > 0, EZeroDeposit);

    let sender = ctx.sender();
    // BUG: this always inserts a fresh balance entry. On a second deposit
    // by the same sender, table::add aborts because the key already exists.
    let bal = coin::into_balance(c);
    table::add(&mut v.balances, sender, bal);
}

/// Withdraw `amount` from the caller's vault balance.
public entry fun withdraw(v: &mut Vault, amount: u64, ctx: &mut TxContext) {
    let sender = ctx.sender();
    let bal = table::borrow_mut(&mut v.balances, sender);
    assert!(balance::value(bal) >= amount, EInsufficientBalance);
    let split = balance::split(bal, amount);
    let coin_out = coin::from_balance(split, ctx);
    transfer::public_transfer(coin_out, sender);
}

/// Read the caller's current balance amount.
public fun balance_of(v: &Vault, who: address): u64 {
    if (!table::contains(&v.balances, who)) return 0;
    balance::value(table::borrow(&v.balances, who))
}
