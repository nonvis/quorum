// Pre-Move-2024 treasury module. Refactor target.
// The benchmark task is to modernize this to Move 2024 idioms while
// preserving behavior exactly.
module treasury::treasury {

    use sui::balance::{Self, Balance};
    use sui::coin::{Self, Coin};
    use sui::sui::SUI;
    use std::vector;

    const EUnauthorized: u64 = 1;
    const EEmpty: u64 = 2;

    public struct Treasury has key {
        id: UID,
        admin: address,
        // History of deposits, in deposit order.
        deposits: vector<u64>,
        balance: Balance<SUI>,
    }

    public struct AdminCap has key, store {
        id: UID,
    }

    fun init(ctx: &mut TxContext) {
        let cap = AdminCap { id: object::new(ctx) };
        let t = Treasury {
            id: object::new(ctx),
            admin: ctx.sender(),
            deposits: vector::empty<u64>(),
            balance: balance::zero<SUI>(),
        };
        transfer::share_object(t);
        transfer::public_transfer(cap, ctx.sender());
    }

    public fun deposit(t: &mut Treasury, c: Coin<SUI>) {
        let amount = coin::value(&c);
        let bal = coin::into_balance(c);
        balance::join(&mut t.balance, bal);
        vector::push_back(&mut t.deposits, amount);
    }

    public fun withdraw(t: &mut Treasury, _cap: &AdminCap, amount: u64, ctx: &mut TxContext): Coin<SUI> {
        let bal = balance::split(&mut t.balance, amount);
        coin::from_balance(bal, ctx)
    }

    // Returns the total amount across all deposits in history.
    public fun get_total_deposited(t: &Treasury): u64 {
        let total = 0;
        let i = 0;
        let len = vector::length(&t.deposits);
        while (i < len) {
            total = total + *vector::borrow(&t.deposits, i);
            i = i + 1;
        };
        total
    }

    public fun get_balance(t: &Treasury): u64 {
        balance::value(&t.balance)
    }

    public fun get_admin(t: &Treasury): address { t.admin }

    // Helper used only within-package.
    public fun rotate_admin(t: &mut Treasury, _cap: &AdminCap, new_admin: address) {
        t.admin = new_admin;
    }

    // Returns the most recent N deposit amounts (or all of them if N is bigger).
    public fun recent_deposits(t: &Treasury, n: u64): vector<u64> {
        let out = vector::empty<u64>();
        let len = vector::length(&t.deposits);
        let start = if (n >= len) { 0 } else { len - n };
        let i = start;
        while (i < len) {
            vector::push_back(&mut out, *vector::borrow(&t.deposits, i));
            i = i + 1;
        };
        out
    }
}
