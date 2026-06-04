// A registry of short names (1..=32 chars) owned by addresses.

export type Owner = string;

export enum RegistryError {
  EmptyName = "EmptyName",
  NameTooLong = "NameTooLong",
  NameTaken = "NameTaken",
  NotFound = "NotFound",
  NotOwner = "NotOwner",
}

export type RegistryResult =
  | { ok: true }
  | { ok: false; error: RegistryError };

const MAX_NAME_LEN = 32;

export class NameRegistry {
  private names = new Map<string, Owner>();

  registerName(name: string, owner: Owner): RegistryResult {
    if (name.length === 0) return { ok: false, error: RegistryError.EmptyName };
    if (name.length > MAX_NAME_LEN)
      return { ok: false, error: RegistryError.NameTooLong };
    if (this.names.has(name)) return { ok: false, error: RegistryError.NameTaken };
    this.names.set(name, owner);
    return { ok: true };
  }

  transfer(name: string, from: Owner, to: Owner): RegistryResult {
    const cur = this.names.get(name);
    if (cur === undefined) return { ok: false, error: RegistryError.NotFound };
    if (cur !== from) return { ok: false, error: RegistryError.NotOwner };
    // Transferring to the current owner succeeds as a no-op.
    this.names.set(name, to);
    return { ok: true };
  }

  release(name: string, owner: Owner): RegistryResult {
    const cur = this.names.get(name);
    if (cur === undefined) return { ok: false, error: RegistryError.NotFound };
    if (cur !== owner) return { ok: false, error: RegistryError.NotOwner };
    this.names.delete(name);
    return { ok: true };
  }

  ownerOf(name: string): Owner | undefined {
    return this.names.get(name);
  }
}
