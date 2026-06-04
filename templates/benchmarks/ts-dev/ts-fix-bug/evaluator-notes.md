# ts-fix-bug — evaluator notes

Bug-diagnosis-then-fix. Items to weight more heavily:

- **Testing** — the regression test for the previously-failing input is the
  primary success signal. It must actually exercise a **second credit to the same
  address** and assert the accumulated total. If the agent's test doesn't hit
  that path, the fix is unverified.
- **Tooling and build** — `tsc --noEmit` clean + all tests green is the floor.
- **Type safety** — amounts stay `bigint` (no `Number(...)` coercion that would
  lose precision on large token amounts); no `any` introduced.

The bug: `credit(addr, amount)` writes with `balances.set(addr, amount)`, which
**overwrites** an existing balance — so a second credit to the same address drops
the prior amount. The existing tests only credit each address once, so they pass
and hide it. A correct fix accumulates:
`balances.set(addr, (balances.get(addr) ?? 0n) + amount)`. It's a control-flow
fix — no signature change.

Items that matter LESS:
- Sui SDK correctness / dApp Kit — N/A; this is a pure data module.
- Test breadth — the existing tests + the second-credit regression is enough;
  don't expect a full input sweep.
