# Quorum — Claude Code Instructions

Deterministic Python tooling: `quorum spend` (token/$ readout from Claude Code
transcripts), the vault index (`quorum q`, `links`, `mentions`, `repos`, `index`)
and the per-repo index (`quorum repo q`, `sym`, `repo index`), plus `quorum
radio <project>` (one project's day on air: specialties answering one question
each from their own slice of the inputs, a script, an MP3), `quorum radio
brainstorm` (one question put to the room) and Radio Studio (`quorum web start`:
a React app over a stdlib API on 127.0.0.1).
What each command does is in `README.md`; the source is the truth.

> Project status, roadmap and decisions live in the maintainer's private design
> vault, not in this repo. The C++ daemon, its web app and the agent/knower model
> were retired on 2026-10-16 — don't reintroduce them. Radio Studio (`radio-web/` +
> `scripts/radio_web.py`) is the one web front, ruled separately: no daemon, no
> agents, owner-started.

## Rules

- **Python standard library only.** No pip dependencies, no build step, no
  compiled code. `sqlite3` with FTS5 is the storage. The non-Python pieces are
  `radio-voice/` (its own Node package; `kokoro-js` pinned exactly) and
  `radio-web/` (Radio Studio's React app; every dependency pinned exactly, built
  by `quorum web start`, served by the stdlib `scripts/radio_web.py`).
- **Zero model calls, except `quorum radio` and `quorum study`.** Every other
  command is deterministic and costs $0. The core (`quorum_cli.py`, `vault_index.py`,
  `repo_index.py`, `spend_readout.py`, `lint.py`) never imports `radio.py` or
  anything audio: the CLI runs radio and study as subprocesses. Radio's tests use its fake
  model and fake TTS — never a real model call in a test.
- **Read-only toward what it reads.** `spend` reads transcripts; the indexes read
  markdown and git. The only files written are `<vault>/.quorum/index.db`,
  `<repo>/.quorum/index.db` and, if missing, `<repo>/.quorum/.gitignore` (`*`);
  `quorum radio` also writes episodes to the per-host store `.quorum/radio/`
  and appends new ids to `radio/voices.lock`; `quorum study` writes only its
  per-host store `.quorum/study/`; Radio Studio writes the vault's
  `06_Metadata/quorum-projects.json` (+ the allowlist) and commits exactly those
  files, and adds brainstorm specialties to `radio/cast.json`.
- **Radio Studio binds 127.0.0.1 only** and refuses a foreign Host or Origin;
  it is started and stopped by the owner (`quorum web start|stop`), never a
  login item or a self-restarting service.
- **Roles are the owner's tuning surface.** `radio/roles/<id>.md` is edited from Radio
  Studio's Roles page (each save is a commit); keep them free of vault paths and private
  facts — the Verifier's owner rules are read from memory at run time, never copied in.
- **Radio voices are bound for life.** `radio/cast.json` gives each specialty
  one Kokoro voice and one pan; never reassign a voice (the lock refuses it),
  never reuse a retired specialty's voice, never change the pinned weights.
  Each specialty reads only its own slice of the inputs (`reads.sources`); keep
  the slices disjoint. The Verifier's reads are read-only git/note reads run by
  the program (`run_read`), never a shell.
- **Walls.** A repo is indexed only if it is on the vault's allowlist or has its
  own `.quorum/`; repo content never leaves its repo. A study track may READ any
  checkout under the roots (work repos included): never indexed, allowlisted,
  recapped or written to, no git command (HEAD is read from `.git/`), and the
  tutor has Read/Grep/Glob only.
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
