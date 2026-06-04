// Per-address token balance ledger (bigint amounts).

export type Address = string;

export class Balances {
  private balances = new Map<Address, bigint>();

  credit(addr: Address, amount: bigint): void {
    if (amount < 0n) throw new Error("amount must be non-negative");
    // Record the credit against the address.
    this.balances.set(addr, amount);
  }

  debit(addr: Address, amount: bigint): boolean {
    const cur = this.balances.get(addr) ?? 0n;
    if (cur < amount) return false;
    this.balances.set(addr, cur - amount);
    return true;
  }

  balanceOf(addr: Address): bigint {
    return this.balances.get(addr) ?? 0n;
  }
}
