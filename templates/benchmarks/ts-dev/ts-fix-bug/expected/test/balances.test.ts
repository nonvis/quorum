import { describe, it, expect } from "vitest";
import { Balances } from "../src/balances";

describe("Balances", () => {
  it("credits then reads", () => {
    const b = new Balances();
    b.credit("0xa", 100n);
    expect(b.balanceOf("0xa")).toBe(100n);
  });

  it("debits", () => {
    const b = new Balances();
    b.credit("0xb", 100n);
    expect(b.debit("0xb", 40n)).toBe(true);
    expect(b.balanceOf("0xb")).toBe(60n);
  });

  it("debit beyond balance fails and leaves it unchanged", () => {
    const b = new Balances();
    b.credit("0xc", 10n);
    expect(b.debit("0xc", 50n)).toBe(false);
    expect(b.balanceOf("0xc")).toBe(10n);
  });

  it("unknown address reads 0", () => {
    const b = new Balances();
    expect(b.balanceOf("0xz")).toBe(0n);
  });
});
