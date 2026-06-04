---
name: ts-ptb
description: Build a Sui PTB script from scratch with the modern @mysten/sui v2 SDK.
---

# TS PTB Benchmark

## Goal

Write a small TypeScript script from scratch that builds and (dry-run) executes a
Programmable Transaction Block against Sui, using the modern `@mysten/sui` v2
SDK. This is the "Hello World" of the Sui SDK surface — exercise the core
patterns cleanly and with correct types.

The script (`src/transfer.ts`) should export a function:
- `buildTransferTx(params: { recipient: string; amountMist: bigint }): Transaction`
  — splits `amountMist` off the gas coin and transfers it to `recipient`, using
  `new Transaction()`, `tx.splitCoins(tx.gas, [...])`, and `tx.transferObjects`.

And a runnable entry that:
- Creates a `SuiGrpcClient` (from `@mysten/sui/grpc`) pointed at a fullnode URL
  read from `process.env` (validated — see constraints).
- Builds the tx with `buildTransferTx`, and calls `client.dryRunTransaction`
  (or `devInspect`) — **do not** broadcast to mainnet.

## Constraints

- `@mysten/sui` (v2) only — never `@mysten/sui.js`. Import via subpath exports
  (`@mysten/sui/transactions`, `@mysten/sui/grpc`, `@mysten/sui/keypairs/ed25519`,
  `@mysten/sui/utils`).
- `SuiGrpcClient`, not the legacy JSON-RPC client.
- `tsconfig` with `strict: true`; **no `any`**; `npx tsc --noEmit` must pass.
- Read the network + fullnode URL from env validated with a Zod schema
  (`z.enum(["mainnet","testnet","devnet","localnet"])`, `z.url()`); never hardcode
  the URL or a package/object ID.
- `amountMist` is `bigint`; pass it through `tx.pure.u64(...)` correctly.

## What to deliver

- `package.json` (deps: `@mysten/sui`, `zod`; devDeps: `typescript`), with a
  `typecheck` script running `tsc --noEmit`.
- `tsconfig.json` (`strict: true`, modern `module`/`moduleResolution`).
- `src/transfer.ts` — `buildTransferTx` + the client/dry-run entry.
- Install (`pnpm install`) and confirm `npx tsc --noEmit` is clean.

The evaluator will score against the ts-dev rubric. Categories that matter most:
Sui SDK correctness (the entire task), Type safety and TS rigor, Config and
secrets (env validation), Tooling and build (clean typecheck).
