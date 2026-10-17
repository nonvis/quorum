# Quorum

Deterministic, local tooling around a Claude Code workspace. Python standard
library only; no build step. One verb calls a model: `quorum radio tonight`
(below); everything else is $0.

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
quorum repo q <terms…> [--repo <name> | --all] [--limit N] [--json]
quorum sym <name> [--repo <name> | --all] [--json]
quorum repo index [--repo <name> | --all] [--rebuild]
quorum lint [--fix-safe] [--json] [--show N]
quorum hook                      # Claude Code UserPromptSubmit hook (stdin JSON)
quorum hook on|off|status
quorum hook --report [--json]
quorum radio inputs [--since <ISO8601>]
quorum radio tonight [--fake] [--topic <words>] [--since <ISO8601>] [--no-voice]
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
  (`scripts/lint.py`, `lint.py`, or a full `~/nonvis/…` path) or a commit (any
  unambiguous prefix; the repo holding it is named). Mentions are `~/nonvis/…` and
  `~/projects/…` paths, backticked repo-relative files, backticked commit hashes,
  and `PR #n` (a bare `#n` is not kept).
- **`repos`** lists every repo the notes name, resolved by git once per root:
  `repo` (worktrees folded into their main checkout; a parent dir holding a repo
  one level down is followed), `not-git`, or `absent` on this host — with origin's
  URL and whether it is on the allowlist, the vault's tracked
  `06_Metadata/quorum-repos.txt` (one repo name per line).
- **`index`** refreshes the index; every read already refreshes first. Memory
  files under `memory/archive/` rank after live ones, like `Archive/` notes.
- **`repo q`** searches a repo's own index at `<repo>/.quorum/index.db`: its
  markdown, its commit messages (subject and body) and its symbol names. **`sym`**
  says where a symbol is defined (case-insensitive prefix; Python, Rust, TS/JS,
  Move, C/C++, Go, shell). **`repo index`** builds or refreshes repo indexes.
  The default target is the repo you are in; outside one, every allowlisted repo
  (`--all`); `--repo <name>` picks one. A repo is indexed only if it is on the
  vault's allowlist or has opted in with its own `.quorum/` directory — so inside
  an opted-in repo these work with no vault at all. The index covers the
  committed tree at HEAD (not uncommitted edits); a refresh re-reads only the
  blobs `git ls-tree` shows changed and the commits after the indexed head, and
  re-reads the history whole after a rewrite. Vendored dirs (`third_party/`,
  `vendor/`, `node_modules/`, `dist/`, `build/`) are listed, not parsed.
  `.quorum/.gitignore` (`*`) keeps the repo clean.
- **`lint`** is a hygiene report (see `scripts/lint.py`): dangling wikilinks,
  orphan notes, live notes without `summary:`, notes naming repo files that are
  gone, memory files missing from (or index rows pointing past) the MEMORY
  indexes, repo docs that point into the vault, repo docs naming files that are
  gone, and notes whose `locked_in: <repo>/<path>` doc is missing. `--fix-safe`
  rewrites only links whose target matches exactly one note after normalising
  case, dashes and spaces. A repo's `docs/` holds what is locked and must read
  without the vault; the vault links to it (`locked_in:`), never the reverse.
- **`q`** also searches the allowlisted repos' `docs/` and root guides, listed
  after the vault hits (`--vault-only` skips them).
- **`hook`** is a Claude Code `UserPromptSubmit` hook. It reads the hook JSON on
  stdin and, only when the prompt clearly matches indexed notes, prints up to five
  of their paths + summaries as `additionalContext` (≤ ~1k tokens). "Clearly":
  the prompt has ≥ 2 *salient* terms (in ≤ 15% of indexed docs; no numbers or
  two-letter words); a hit covers ≥ 2 of them and at least a third, and names
  one in its title, summary opening or path — two, when the prompt has ≤ 3. It
  never blocks a prompt: any error exits 0 silently; `QUORUM_HOOK=off` disables
  it. Each call appends one line to `<vault>/.quorum/hook.log` (session, term
  count, hit paths — never the prompt). Install, in a project's
  `.claude/settings.json`: `"command": "quorum hook 2>/dev/null || true"`.
- **`hook off` / `on` / `status`** turn the hook off or on for every session in the
  vault (a flag file, `<vault>/.quorum/hook-off`); `QUORUM_HOOK=off claude` does
  it for one shell's sessions. While off, the hook injects nothing but still logs
  the call with `"mode":"off"`, so the report can compare.
- **`hook --report`** reads that log and the sessions' transcripts: prompts seen,
  injections, how many injected notes the session then Read, how many sessions
  ran a `quorum` lookup themselves, and per mode (on / off) the sessions, prompts
  and measured main-session context per turn — an A/B of the hook's cost.

## Radio

`quorum radio` puts one owner's *desks* on air: a short spoken episode in which
specialties — one per project (Meridian, Permafrost, Sidebar, Bastion), plus a
Skeptic (the verification rules) and an Accountant (spend and what is owed) —
answer a host. The listener is the owner, so there is no screen or trust gate.

- **`radio/cast.json`** (tracked) is the cast: per specialty `{id, name, voice,
  pan, reads}`. Each specialty is bound to **one Kokoro voice and one stereo
  position for its lifetime**, so the owner learns who is talking by ear.
  `radio/voices.lock` records every voice ever held: the loader refuses a
  duplicate voice, a voice outside Kokoro's English presets, a changed voice for
  a locked id, and a retired id's voice for a new one.
- **`radio inputs`** ($0) prints JSON: per desk, the briefing note(s) it reads
  (with mtime and section headings), the commit subjects in its repos (only those
  on the vault allowlist; others are listed as skipped), the `spend:` close lines
  whose window ends inside the window, and the Action Queue items whose title
  names it. The Skeptic reads the top-severity verification rules from the
  memory dir; the Accountant reads every desk's spend lines and the briefings'
  Owed rows. The window defaults to the last 24 h. A desk with no activity
  (commits, spend lines, a note updated) is off the cast; the Skeptic and the
  Accountant always sit.
- **`radio tonight`** runs the episode, a text round: the host asks (the cast,
  or one desk), each desk asked answers in writing citing one of its own sources
  by id (a note section, a commit, a spend line, an Action Queue item, a rule,
  or another desk's line on air) or passes; an answer whose source does not
  resolve is a pass. The host picks one answer, names the desk and logs a
  one-line reason; the written answer is the on-air line, with its kind said
  aloud ("I know…", "My guess is…", "The Meridian desk says…"). A repeat-back is
  put to the desk in the next call (confirm, correct, or withdraw — a withdrawn
  claim leaves the audio). Another question only while the last question to the
  cast drew ≥ 2 sourced answers with new material; 14 calls at most, the last
  one the host's sign-off. The model is `claude -p --model claude-sonnet-5-5`
  (no tools, safe mode) run from `~/.cache/quorum/radio/session`, so
  `quorum spend --project ~/.cache/quorum/radio/session` meters it; `--fake`
  uses a deterministic stand-in. If the first call fails nothing is written.
- The episode is written to `<vault>/01_Projects/Quorum/Radio/NN - <Topic>
  (YYYY-MM-DD).md` (cast, transcript with speaker / voice / line / source /
  the host's pick reason, the program log, the call count, the spend line) and
  voiced to an MP3 beside it (`--no-voice` skips it). Play it with `afplay`.
- **`radio-voice/`** is the voice sidecar, a separate Node package
  (`kokoro-js` pinned to 1.2.1, `package-lock.json` tracked, `node_modules/`
  not): `node render.mjs --cast radio/cast.json <job.json> <out.mp3>`. It voices
  one line at a time in the speaker's bound voice, pans it to the bound
  position, inserts the pauses the job gives (computed from the script's
  structure, never by the model), and encodes one MP3 with `ffmpeg`. It refuses
  a line whose voice is not its speaker's in the cast file. Weights: Kokoro-82M
  v1.0 ONNX q8 in `~/.cache/quorum/kokoro` (`QUORUM_KOKORO_CACHE`), checked
  against `radio-voice/weights.sha256` and never re-fetched once present.
  Setup: `cd radio-voice && npm ci`.

The stdlib core never imports radio: `quorum radio` runs `scripts/radio.py` as a
subprocess, and the TTS lives in the sidecar.

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
scripts/repo_index.py      the per-repo index (and its tests)
scripts/lint.py            quorum lint (and its tests)
scripts/radio.py           quorum radio: cast, inputs, the round, the note (and its tests)
radio/cast.json            the cast: bound voice + pan per specialty; radio/voices.lock
radio-voice/               the Node voice sidecar (Kokoro + ffmpeg)
scripts/install.sh, doctor.sh
templates/skills/          the three linked skills
```
