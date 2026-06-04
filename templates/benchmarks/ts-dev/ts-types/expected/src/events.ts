// Parses Sui-style on-chain event payloads.
//
// Currently typed with `any` throughout and narrowed by ad-hoc checks.
// Tighten this to a proper discriminated union (no `any`, no casts) without
// changing behavior.

export function parseEvent(raw: any): any {
  if (raw.kind === "transfer") {
    return {
      kind: "transfer",
      from: raw.from,
      to: raw.to,
      amount: BigInt(raw.amount),
    };
  }
  if (raw.kind === "mint") {
    return { kind: "mint", to: raw.to, amount: BigInt(raw.amount) };
  }
  if (raw.kind === "burn") {
    return { kind: "burn", from: raw.from, amount: BigInt(raw.amount) };
  }
  throw new Error("unknown event kind: " + raw.kind);
}

export function summarize(e: any): string {
  switch (e.kind) {
    case "transfer":
      return e.from + " -> " + e.to + " (" + e.amount + ")";
    case "mint":
      return "mint " + e.amount + " -> " + e.to;
    case "burn":
      return "burn " + e.amount + " from " + e.from;
    default:
      return "unknown";
  }
}
