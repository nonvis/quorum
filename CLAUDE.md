# Quorum — Claude Code Instructions

Deterministic Python tooling: `quorum spend` (token/$ readout from Claude Code
transcripts), the vault index (`quorum q`, `links`, `mentions`, `repos`, `index`)
and the per-repo index (`quorum repo q`, `sym`, `repo index`), plus `quorum
radio <project>` (one project's day on air: specialties answering one question
each from their own slice of the inputs, a note, an MP3).
What each command does is in `README.md`; the source is the truth.

> Project status, roadmap and decisions live in the maintainer's private design
> vault, not in this repo. The C++ daemon, the web app and the agent/knower model
> were retired on 2026-10-16 — don't reintroduce them.

## Rules

- **Python standard library only.** No pip dependencies, no build step, no
  compiled code. `sqlite3` with FTS5 is the storage. The one non-Python piece is
  `radio-voice/` (its own Node package; `kokoro-js` pinned exactly).
- **Zero model calls, except `quorum radio <project>`.** Every other command is
  deterministic and costs $0. The core (`quorum_cli.py`, `vault_index.py`,
  `repo_index.py`, `spend_readout.py`, `lint.py`) never imports `radio.py` or
  anything audio: the CLI runs radio as a subprocess. Radio's tests use its fake
  model and fake TTS — never a real model call in a test.
- **Read-only toward what it reads.** `spend` reads transcripts; the indexes read
  markdown and git. The only files written are `<vault>/.quorum/index.db`,
  `<repo>/.quorum/index.db` and, if missing, `<repo>/.quorum/.gitignore` (`*`);
  `quorum radio <project>` also writes its episode note + MP3 under the vault's
  `01_Projects/Quorum/Radio/` and appends new ids to `radio/voices.lock`.
- **Radio voices are bound for life.** `radio/cast.json` gives each specialty
  one Kokoro voice and one pan; never reassign a voice (the lock refuses it),
  never reuse a retired specialty's voice, never change the pinned weights.
  Each specialty reads only its own slice of the inputs (`reads.sources`); keep
  the slices disjoint. The Verifier's reads are read-only git/note reads run by
  the program (`run_read`), never a shell.
- **Walls.** A repo is indexed only if it is on the vault's allowlist or has its
  own `.quorum/`; repo content never leaves its repo.
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
