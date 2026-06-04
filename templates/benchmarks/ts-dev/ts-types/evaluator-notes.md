# ts-types — evaluator notes

This task is exclusively about type rigor. Items to weight more heavily:

- **Type safety and TS rigor** — the whole point. Score strictly:
  - **Zero `any`** in `src/events.ts` afterward — explicit `any`, `as any`, and
    implicit-any all count. Grep the result.
  - The payloads are modeled as a **discriminated union** (shared literal tag),
    and narrowing uses `Extract<...>` or a `switch`/`if` on the tag — NOT `as`
    casts or `!`. A solution that just renames `any` to `unknown` and casts is a
    miss.
  - Inference flows: the parser's return type is the precise variant, not a wide
    union the caller must re-narrow with casts.
- **Tooling and build** — `tsc --noEmit` clean AND `vitest run` green. Behavior
  must be unchanged; if the agent altered an assertion to make a test pass, that's
  a comprehension failure — the task was types-only.

Acceptable latitude:
- `type` vs `interface` for the variants — either is fine if consistent.
- Using a `switch (e.kind)` vs `Extract<Event, { kind: "..." }>` — both are valid
  narrowing; don't prefer one.

Items that matter LESS:
- Sui SDK correctness — the payloads are plain data; no live SDK call here.
- dApp Kit frontend — N/A.
- Testing breadth — the suite exists and stays green; the agent isn't adding
  coverage, just keeping it compiling.
