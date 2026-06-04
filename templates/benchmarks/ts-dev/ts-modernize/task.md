---
name: ts-modernize
description: Modernize legacy-style TypeScript to modern ESM/strict idioms, preserving behavior.
---

# TS Modernize Benchmark

## Goal

The starter module at `src/format.ts` is working but written in a dated style:
`var`, loose equality (`==`), `any`, `Promise.then` chains, index `for` loops, a
`namespace`, and a default export. Refactor it to modern, strict TypeScript while
preserving behavior exactly.

Specifically modernize:
- `var` → `const` / `let`.
- `==` / `!=` → `===` / `!==`.
- `any` → real types (the inputs are well-defined).
- `Promise.then(...).catch(...)` chains → `async` / `await` with `try/catch`.
- index `for (var i = 0; ...)` loops over arrays → array methods (`map` / `filter`
  / `reduce`) or `for...of` where clearer.
- `namespace Foo { ... }` → ESM module-level named exports.
- `export default` → named exports (update the test import accordingly).

## Constraints

- Behavior MUST NOT change — same outputs for the same inputs, same error
  conditions. The existing tests in `test/format.test.ts` must keep passing
  (`npx vitest run`); update only the import line if you change the export style.
- `npx tsc --noEmit` clean under `strict: true`; no `any` remaining.
- Don't add new functionality — modernize only.
- No `var`, no `==`/`!=`, no `namespace`, no default export remaining.

## What to deliver

- Modernized `src/format.ts` (named exports, `const`/`let`, `===`, typed,
  async/await, array methods).
- `test/format.test.ts` updated only for the new import style (same assertions).
- `npx tsc --noEmit` clean and `npx vitest run` green.

The evaluator will score against the ts-dev rubric. Categories that matter most:
Type safety and TS rigor, Structure and conventions (modern idioms, named
exports), Tooling and build (regression-free).
