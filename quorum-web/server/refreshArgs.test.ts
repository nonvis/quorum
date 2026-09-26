import { test, expect } from "bun:test";
import { refreshArgs, KNOWER_NAMES } from "./refreshArgs";

test("default (empty body) → --all", () => {
  expect(refreshArgs()).toEqual({ ok: true, args: ["--all"] });
  expect(refreshArgs({})).toEqual({ ok: true, args: ["--all"] });
  expect(refreshArgs({ knower: "" })).toEqual({ ok: true, args: ["--all"] });
  expect(refreshArgs({ knower: "all" })).toEqual({ ok: true, args: ["--all"] });
});

test("one lens → --knower <name>, for each of the four", () => {
  for (const k of KNOWER_NAMES) {
    expect(refreshArgs({ knower: k })).toEqual({ ok: true, args: ["--knower", k] });
  }
});

test("a stray parallel flag is ignored — lenses always run in turn", () => {
  expect(refreshArgs({ parallel: true } as never)).toEqual({ ok: true, args: ["--all"] });
});

test("unknown lens name → rejected, and the error lists the valid ones", () => {
  const r = refreshArgs({ knower: "cartographerr" });
  expect(r.ok).toBe(false);
  expect(r.ok === false && r.error).toContain("unknown knower: cartographerr");
  expect(r.ok === false && r.error).toContain("cartographer | architect | historian | recap");
});

// Liveness for the refusal: one step the other side of it must PASS.
test("the refusals do not overfire", () => {
  // a real lens passes …
  expect(refreshArgs({ knower: "historian" })).toEqual({
    ok: true,
    args: ["--knower", "historian"],
  });
  // … and only a bad name is rejected.
  const r = refreshArgs({ knower: "nope" });
  expect(r.ok === false && r.error).toContain("unknown knower");
});

test("--all and --knower are never both emitted", () => {
  for (const req of [{}, { knower: "recap" }, { knower: "all" }]) {
    const r = refreshArgs(req);
    if (!r.ok) continue;
    expect(r.args.includes("--all") && r.args.includes("--knower")).toBe(false);
  }
});
