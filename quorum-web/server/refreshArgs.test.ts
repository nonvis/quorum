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

test("parallel with --all → --all --parallel", () => {
  expect(refreshArgs({ parallel: true })).toEqual({ ok: true, args: ["--all", "--parallel"] });
  expect(refreshArgs({ knower: "all", parallel: true })).toEqual({
    ok: true,
    args: ["--all", "--parallel"],
  });
});

test("parallel with a single lens → rejected (the daemon rejects it too)", () => {
  const r = refreshArgs({ knower: "historian", parallel: true });
  expect(r.ok).toBe(false);
  expect(r.ok === false && r.error).toContain("--parallel requires --all");
});

test("unknown lens name → rejected, and the error lists the valid ones", () => {
  const r = refreshArgs({ knower: "cartographerr" });
  expect(r.ok).toBe(false);
  expect(r.ok === false && r.error).toContain("unknown knower: cartographerr");
  expect(r.ok === false && r.error).toContain("cartographer | architect | historian | recap");
});

// Liveness for the two refusals: one step the other side of each must PASS.
test("the refusals do not overfire", () => {
  // parallel:false with a lens is fine …
  expect(refreshArgs({ knower: "historian", parallel: false })).toEqual({
    ok: true,
    args: ["--knower", "historian"],
  });
  // … and a bad name with parallel is still rejected for the NAME first,
  // the daemon's own order (knower_refresh.h :265 before :274).
  const r = refreshArgs({ knower: "nope", parallel: true });
  expect(r.ok === false && r.error).toContain("unknown knower");
});

test("--all and --knower are never both emitted", () => {
  for (const req of [{}, { parallel: true }, { knower: "recap" }, { knower: "all" }]) {
    const r = refreshArgs(req);
    if (!r.ok) continue;
    expect(r.args.includes("--all") && r.args.includes("--knower")).toBe(false);
  }
});
