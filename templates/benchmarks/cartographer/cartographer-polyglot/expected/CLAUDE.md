# Sweep Workspace

A small workspace for a sweeping liquidation pipeline.

## Layout

- `sweep-service/` — the Rust off-chain service that watches positions and
  triggers sweeps. Entry point in `src/main.rs`.
- `sweep-contract/` — the Sui Move on-chain contract that records sweep
  events and holds the vault registry.
- `config/` — runtime configuration (RPC endpoints, thresholds). Read-only;
  do not edit.

## Rules

- Read-only orientation only. Do not modify any file.
