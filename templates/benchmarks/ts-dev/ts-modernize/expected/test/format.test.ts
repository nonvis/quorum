import { describe, it, expect } from "vitest";
import FormatImpl from "../src/format";

describe("format", () => {
  it("formats a list", () => {
    expect(FormatImpl.formatList([1, 2, 3])).toBe("1, 2, 3");
  });

  it("sums numbers", () => {
    expect(FormatImpl.sum([1, 2, 3])).toBe(6);
  });

  it("echoes asynchronously", async () => {
    expect(await FormatImpl.delayedEcho("hi")).toBe("echo: hi");
  });
});
