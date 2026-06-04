import { describe, it, expect } from "vitest";
import { parseEvent, summarize } from "../src/events";

describe("parseEvent", () => {
  it("parses a transfer", () => {
    const e = parseEvent({
      kind: "transfer",
      from: "0xa",
      to: "0xb",
      amount: "100",
    });
    expect(e.kind).toBe("transfer");
    expect(e.amount).toBe(100n);
  });

  it("parses a mint and summarizes it", () => {
    const e = parseEvent({ kind: "mint", to: "0xb", amount: "5" });
    expect(summarize(e)).toBe("mint 5 -> 0xb");
  });

  it("throws on an unknown kind", () => {
    expect(() => parseEvent({ kind: "nope" })).toThrow();
  });
});
