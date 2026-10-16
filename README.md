# Quorum

Deterministic, local tooling around a Claude Code workspace. Python standard
library only; no build step; it never calls a model.

Quorum began as a C++ daemon that orchestrated Claude Code agents with a web
dashboard. The daemon, the web app and the agent model were retired on
2026-10-16; git history keeps them.

## Commands

```
quorum spend [--project <path|name>] [--since <ISO8601>] [--until <ISO8601>]
             [--session <id|prefix>] [--json | --line]
quorum q <terms…> [--limit N] [--source vault|memory] [--json]
quorum links <note> [--json]
quorum mentions <repo|~/path|file|sha> [--json]
quorum repos [--json]
quorum index [--stats] [--rebuild]
quorum version
```

- **`spend`** reads token use from Claude Code's transcript JSONLs and prices it
  at list rates (an estimate, labelled `EST`). `--session <id>` scopes the read to
  one main transcript plus its own subagents; its window runs from the session's
  first record to its last. `--line` prints the one-line `spend:` close record.
  `--project` takes a directory, or a name looked up under `~/projects` and
  `~/nonvis` (the first holding a `.quorum/`); the default is the current
  directory.
- **`q`** is ranked full-text search (SQLite FTS5) over the vault's notes and its
  Claude Code memory files. Any term matches, as a prefix; results rank by how
  many terms a note covers, live notes before archived ones, then bm25 (title ·
  summary · tags · body weighted 10 · 5 · 3 · 1).
- **`links`** lists a note's outgoing `[[wikilinks]]` (resolved by file name, or
  dangling), its backlinks, and what it mentions outside the vault.
- **`mentions`** lists the notes that name a repo (by name or `~/` path), a file
  (`scripts/run.py`, `run.py`, or a full `~/nonvis/…` path) or a commit (any
  unambiguous prefix; the repo holding it is named). Mentions are `~/nonvis/…` and
  `~/projects/…` paths, backticked repo-relative files, backticked commit hashes,
  and `PR #n` (a bare `#n` is not kept).
- **`repos`** lists every repo the notes name, resolved by git once per root:
  `repo` (worktrees folded into their main checkout; a parent dir holding a repo
  one level down is followed), `not-git`, or `absent` on this host — with origin's
  URL and whether it is on the allowlist, the vault's tracked
  `06_Metadata/quorum-repos.txt` (one repo name per line).
- **`index`** refreshes the index; `q` and `links` already refresh first.

The vault defaults to `~/nonvis/second-brain` (`QUORUM_VAULT` overrides); its
memory directory is Claude Code's for that path (`QUORUM_MEMORY` overrides). The
index lives at `<vault>/.quorum/index.db` and is never tracked. Each refresh stats
every markdown file and re-reads only those whose mtime or size changed, so the
index can't be stale when it is read and nothing has to be scheduled.

## Setup

```bash
make install   # ~/.local/bin/quorum -> scripts/quorum_cli.py, plus three skills into ~/.claude/skills
make doctor    # read-only check
make test      # every scripts/*_test.py
```

The three skills linked into `~/.claude/skills` (`sui-dev-skills`,
`move-code-quality`, `cpp-code-quality`) live under `templates/skills/`.

## Layout

```
scripts/quorum_cli.py      the `quorum` command
scripts/spend_readout.py   spend (and its tests)
scripts/vault_index.py     the vault index (and its tests)
scripts/install.sh, doctor.sh
templates/skills/          the three linked skills
```
