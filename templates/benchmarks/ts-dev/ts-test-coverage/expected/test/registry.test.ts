import { describe, it, expect } from "vitest";
import { NameRegistry, RegistryError } from "../src/registry";

// TODO: this suite is a STUB. Expand it to cover the happy path for every
// exported function, boundary inputs (empty / 1 / 32 / 33-char names, missing
// lookup), and every failure case (NameTaken, NotOwner, NotFound), asserting the
// specific RegistryError values. See task.md.

describe("NameRegistry", () => {
  it("registers a name", () => {
    const r = new NameRegistry();
    expect(r.registerName("alice", "0xa").ok).toBe(true);
    expect(r.ownerOf("alice")).toBe("0xa");
  });
});

// silence unused-import lint until the suite is expanded to assert error values
void RegistryError;
