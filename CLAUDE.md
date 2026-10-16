# Quorum — Claude Code Instructions

Deterministic Python tooling: `quorum spend` (token/$ readout from Claude Code
transcripts) and the vault index (`quorum q`, `quorum links`, `quorum index`).
What each command does is in `README.md`; the source is the truth.

> Project status, roadmap and decisions live in the maintainer's private design
> vault, not in this repo. The C++ daemon, the web app and the agent/knower model
> were retired on 2026-10-16 — don't reintroduce them.

## Rules

- **Python standard library only.** No pip dependencies, no build step, no
  compiled code. `sqlite3` with FTS5 is the storage.
- **Zero model calls.** Every command is deterministic and costs $0.
- **Read-only toward what it reads.** `spend` reads transcripts; the index reads
  markdown. The only file written is `<vault>/.quorum/index.db`.
- **Self-repairing index.** A query refreshes first (stat + re-parse what changed);
  never add a scheduler, daemon or watcher.
- **Tests first.** Each change gets a test that fails on the old code, then the
  fix. Tests are standalone `unittest` files: `scripts/<module>_test.py`, run by
  `make test`. Use real temp dirs, not mocks.
- `~/.local/bin/quorum` is a symlink to `scripts/quorum_cli.py`, so a change is
  live as soon as it is saved.

## Repo habits

Commits go straight to `main`; one-line messages; push only when the maintainer
asks.
