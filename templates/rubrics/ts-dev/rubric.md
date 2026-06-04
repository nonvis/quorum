---
name: ts-dev
version: v1
---

# Rubric: ts-dev (v1)

Sources: the `sui-ts-sdk` + `sui-frontend` skills (this repo), and the conventions
distilled from the MystenLabs `dapp-template` — the official full-stack scaffold
(Next.js 16 frontend + Move package + publish CLI + Vitest/TestContainers e2e,
pnpm workspaces, React 19, TypeScript strict). The bar is **full-stack TypeScript
for Sui**: SDK/PTB craft + dApp Kit frontend + config-as-code + strict typing +
the tooling that keeps it honest.

Calibration intent:
- **Strict TypeScript is the floor.** `strict: true`, no `any` (narrow, don't cast).
- **Modern stack is required for new code:** `@mysten/sui` v2 (not `@mysten/sui.js`),
  `SuiGrpcClient`, `@mysten/dapp-kit-react` v2. Legacy clients PASS only when the
  project already standardizes on them (judgment — document).
- **Frontend items (dApp Kit) are N/A for a backend-only change** — say so; don't
  fail a server script for lacking React.
- **Match the project.** Framework (Next/Vite/plain TS), package manager, and
  structure follow the surrounding repo — scored as "consistent with the
  surrounding code," not a fixed value.
- **Stub-only test files default to FAIL**, not N/A.
- The `dapp-template` is thin on frontend tests + error handling — the rubric
  scores those as real gaps, not assumed-modeled.

The `evaluator` reads this file, walks each item, and emits per-item pass/fail in
the EVALUATION block. Categories are documentation; per-item `(W)` weights drive
scoring.

## Type safety and TS rigor (weight 18)
- [ ] (5) `tsconfig` has `strict: true` (or stricter); no strict flag is relaxed to land the change
- [ ] (5) No `any` in new code — SDK/union types narrowed with `Extract<...>` or discriminated unions. PASS for a rare unavoidable interop cast carrying an inline justification (e.g. `as any` with an `// eslint-disable` reason)
- [ ] (4) Type-only imports use `import type`; props/aliases declared with `type` or `interface` consistently
- [ ] (4) No unsafe non-null assertions (`!`) or `as` casts on caller-/SDK-supplied data without a narrowing guard first

## Sui SDK correctness (weight 18)
- [ ] (5) Uses `@mysten/sui` (never the deprecated `@mysten/sui.js`), imported via subpath exports (`@mysten/sui/transactions`, `/grpc`, `/keypairs/ed25519`, `/utils`)
- [ ] (4) New clients use `SuiGrpcClient` (`@mysten/sui/grpc`). Legacy JSON-RPC client PASSes only when the project already standardizes on it — judgment, document
- [ ] (5) PTBs built with `new Transaction()`; inputs via `tx.object()` / `tx.pure.*()` / `coinWithBalance`, not hand-rolled byte juggling; gas/budget left to the SDK unless a reason is documented
- [ ] (4) Execution checks status/effects (`signAndExecuteTransaction` with effects/options) and surfaces a failed effect — no fire-and-forget that ignores failure

## dApp Kit frontend (weight 15)

> **N/A** for a backend-only / non-React change — state that; don't fail a server script for lacking a UI.

- [ ] (4) Providers wired with `createDAppKit` + `DAppKitProvider` (`@mysten/dapp-kit-react`) under a `QueryClientProvider`; network/client created in `createDAppKit`'s `createClient`
- [ ] (4) On-chain reads/writes go through dApp Kit hooks (`useSuiClientQuery` / `useCurrentAccount` / `useSignAndExecuteTransaction`), not an ad-hoc client instantiated inside a component
- [ ] (3) Interactive/browser components marked `"use client"`; wallet UI (`ConnectButton`) loaded with `ssr: false`
- [ ] (4) Wallet-gated UI handles the no-account state; affected reads are invalidated/refetched after a mutating tx

## Config and secrets (weight 14)
- [ ] (5) Environment validated through a Zod schema (`z.enum` for network, `z.url` for the fullnode) and parsed at startup
- [ ] (5) No hardcoded network strings, package/object IDs, or fullnode URLs in code — all sourced from validated config
- [ ] (4) No secrets/keys committed or logged; secret-key parsing validates format/length (e.g. `Ed25519Keypair.fromSecretKey` with a length/prefix check)

## Tooling and build (weight 15)
- [ ] (5) `tsc --noEmit` passes with zero type errors
- [ ] (4) ESLint passes under the project config (e.g. next/core-web-vitals + typescript) with no new errors
- [ ] (3) Prettier-clean under the project config (80 col / 2-space / double quotes / trailing commas, unless the repo overrides)
- [ ] (3) Project builds / dev-runs (`next build`, `vite build`, or `tsc`) with no new failures

## Testing (weight 12)

> **Default rule:** a test file that exists but exercises nothing (stub-only) defaults to **FAIL**. N/A is reserved for a change with genuinely nothing testable.

- [ ] (4) Vitest tests exist for new/changed logic, wired into the project's `test` script
- [ ] (4) Coverage includes the happy path, the failure path, and boundaries (empty, max, missing, malformed input)
- [ ] (2) Tests are deterministic; any e2e against a network uses isolation (TestContainers / localnet), never mainnet. N/A if there's no e2e surface
- [ ] (2) Tests use real types (no `any` in tests) and assert values, not just truthiness

## Structure and conventions (weight 8)
- [ ] (3) Functional components + hooks (no class components); components `PascalCase` with named exports (default export reserved for framework entry points, e.g. Next pages)
- [ ] (3) Async paths handle rejection (try/catch or an error state); no unhandled promise or swallowed `await` error
- [ ] (2) Matches the project's framework, structure, and package manager; path aliases (`@/*`) used where the repo defines them — judgment, consistent with the surrounding code
