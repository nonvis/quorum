# ts-ptb — evaluator notes

The SDK benchmark. A well-prepared ts-dev agent should clear 80+. Items to weight
more heavily than the default rubric:

- **Sui SDK correctness** — carries this task.
  - `@mysten/sui` v2 with subpath imports; **any** `@mysten/sui.js` import is a
    hard miss.
  - `SuiGrpcClient` from `@mysten/sui/grpc` (not the legacy JSON-RPC client).
  - PTB built with `new Transaction()`; the split + transfer use
    `tx.splitCoins(tx.gas, [tx.pure.u64(amountMist)])` + `tx.transferObjects([...],
    tx.pure.address(recipient))` — not hand-rolled inputs. Gas budget left to the
    SDK.
  - Dry-run / devInspect path — the script must NOT broadcast to mainnet.
- **Config and secrets** — the env (network + fullnode URL) MUST be Zod-validated;
  a hardcoded URL or package ID is a miss even if it "works."
- **Tooling and build** — `npx tsc --noEmit` clean is the floor. If it doesn't
  typecheck, the SDK usage is wrong regardless of how it reads.

Acceptable latitude:
- `dryRunTransaction` vs `devInspectTransactionBlock` — either is fine; what
  matters is no mainnet broadcast.
- Whether the keypair is created or omitted (dry-run needs no signature) — don't
  dock for a keypair that's present but unused, or absent.

Items that matter LESS:
- dApp Kit frontend — N/A; this is a backend script.
- Testing — a typecheck-clean script is the bar here; a Vitest test is a bonus,
  not required for this task.
