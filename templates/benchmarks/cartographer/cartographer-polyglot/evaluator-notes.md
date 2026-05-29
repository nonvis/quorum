# cartographer-polyglot — evaluator notes

The fixture workspace (copied into the temp project from `expected/`) IS the
ground truth. Score the index by walking it against the actual filesystem with
Read / Grep / Glob — no rigid `expected/` output to diff, because the index is
open-form. This is the easy cartographer benchmark; a well-prepared
cartographer should clear 80+.

## Ground-truth layout (what the index MUST cover)

Top-level entries at the workspace root:

| Entry | Kind | Purpose (ground truth) | Key files |
|-------|------|------------------------|-----------|
| `sweep-service/` | service (Rust) | off-chain service that watches positions and triggers sweeps | manifest `sweep-service/Cargo.toml`; entry point `sweep-service/src/main.rs`; also `sweep-service/src/watcher.rs` |
| `sweep-contract/` | contract (Sui Move) | on-chain contract recording sweep events + vault registry | manifest `sweep-contract/Move.toml`; source `sweep-contract/sources/registry.move` |
| `config/` | config | runtime configuration (RPC endpoints, thresholds) | `config/thresholds.toml` |
| `CLAUDE.md` | orientation anchor | workspace-level layout description + read-only rule | (root file) |

There are exactly THREE top-level folders (`sweep-service`, `sweep-contract`,
`config`) plus the root `CLAUDE.md`. An index that misses any folder, or that
invents a fourth folder, fails Top-level coverage.

## Lookup answers (ground truth)

1. Build manifest for the Rust service → `sweep-service/Cargo.toml`.
2. On-chain contract folder → `sweep-contract/`; its manifest is
   `sweep-contract/Move.toml`.

## Scoring emphasis

- **Top-level coverage** + **Key-file/location accuracy** carry the task. Every
  cited path must exist; `Cargo.toml`, `Move.toml`, `main.rs`, `registry.move`,
  `thresholds.toml` are all real — penalize any fabricated path hard.
- **CLAUDE.md honored** — the root `CLAUDE.md` describes all three folders. The
  index should seed from it and refine; the code does NOT contradict it here, so
  there should be no contradiction flag. Note the read-only rule was obeyed
  (the cartographer must not have edited anything).
- **Right altitude** — a correct index stops at the folder level + key files. An
  index that lists every `.rs` file or that starts describing how the service
  calls the contract (interconnection) is wrong altitude / out of scope.
- **Lookup structure** — the two lookup answers must come from the structured
  index, not a fresh scan narrated in prose.

## Items that matter LESS

- Freshness + clarity is only 5 points; a missing staleness stamp is a small
  dock, not a task failure.
