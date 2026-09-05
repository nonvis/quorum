import { test, expect } from "bun:test";
import { conversationBadges, BADGE_AMBER } from "./conversationBadges";

const labels = (c: Parameters<typeof conversationBadges>[0]) =>
  conversationBadges(c).map((b) => b.label);

test("no flags set → no chips (absent ≠ a chip)", () => {
  expect(conversationBadges({})).toEqual([]);
  expect(conversationBadges({ no_vault_write: 0, gated: 0, gate_cleared: 0, team: null })).toEqual([]);
  expect(conversationBadges({ team: "   " })).toEqual([]);
});

test("no_vault_write=1 → read-only vault", () => {
  expect(labels({ no_vault_write: 1 })).toEqual(["read-only vault"]);
});

test("gated=1, gate_cleared=0 → gated · pending", () => {
  expect(labels({ gated: 1, gate_cleared: 0 })).toEqual(["gated · pending"]);
});

test("gated=1, gate_cleared=1 → gated · cleared", () => {
  expect(labels({ gated: 1, gate_cleared: 1 })).toEqual(["gated · cleared"]);
});

test("gated=0 → no gate chip, whatever gate_cleared says", () => {
  expect(labels({ gated: 0, gate_cleared: 1 })).toEqual([]);
});

test("team non-empty → team: <name>", () => {
  expect(labels({ team: "reviewers" })).toEqual(["team: reviewers"]);
});

test("amber is the pending human gate ONLY", () => {
  const pending = conversationBadges({ gated: 1, gate_cleared: 0 })[0]!;
  expect(pending.color).toBe(BADGE_AMBER);

  const others = conversationBadges({
    no_vault_write: 1,
    gated: 1,
    gate_cleared: 1,
    team: "reviewers",
  });
  expect(others).toHaveLength(3);
  for (const b of others) expect(b.color).not.toBe(BADGE_AMBER);
});

test("all four at once → three chips in a stable order", () => {
  expect(labels({ no_vault_write: 1, gated: 1, gate_cleared: 0, team: "reviewers" })).toEqual([
    "read-only vault",
    "gated · pending",
    "team: reviewers",
  ]);
});
