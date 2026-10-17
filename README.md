# Quorum

Deterministic, local tooling around a Claude Code workspace. Python standard
library only; no build step. One verb calls a model: `quorum radio <project>`
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
quorum radio <project> [today | yesterday | week | YYYY-MM-DD | YYYY-MM-DD..YYYY-MM-DD]
             [--with operator] [--fake] [--no-voice] [--since <ISO8601>] [--until <ISO8601>]
quorum radio inputs <project> [range] [--since <ISO8601>] [--until <ISO8601>]
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

`quorum radio <project>` is a short spoken recap of **one project's day** for its
owner, run by hand (some projects get no work on a given day). The listener is
the owner, so there is no screen or trust gate.

- **`radio/projects.json`** (tracked) maps each project to its briefing note(s),
  its vault folder(s), its concept note and its repos. Radio reads the repos on
  this list by path (under `~/nonvis` or `~/projects`), whatever the index
  allowlist says; a project may list none.
- **The range** is `today` (the default: KST midnight → now), `yesterday`, `week`
  (the last 7 days including today), a date, or `A..B` (inclusive, KST). A range
  that ends before now is read as the vault's git held it then (`--since` /
  `--until` set exact stamps). With no commit in the project's repos and no note
  changed in its vault folders, it refuses at $0: `nothing on <project> today`.
  A range longer than a day is a **digest**: the summaries of the notes changed,
  the decision notes' added lines, commits per day with subjects grouped by theme,
  and the transcripts of any daily episodes recorded in it; the Builder speaks in
  at most 3 themes and the host opens with the arc of the period.
- **`radio/cast.json`** (tracked) is the cast: a host and five specialties, the
  same for every project. Each answers **one question**, from **a slice of the
  inputs no other specialty reads**, with a **never-says** list:

  | Specialty | Its question | Its slice |
  |---|---|---|
  | Builder | What actually changed today? | the window's commits and the code files they touched (a range: ≤ 3 themes + commits per day) |
  | Verifier | Is that actually proven? | the evidence: build logs, walks, gates, tests changed today, the verification rules, and what it reads itself |
  | Architect | What did we decide today, and what did it cost? | the lines design / ruling notes gained in the window (another project's decision note counts when those lines name this project), the briefing's §Rulings when it changed |
  | Product | Does a user notice any of it? | the diffs of user-facing files (docs, UI, CLI help), surface notes, the concept note |
  | Operator | What's waiting on the owner? | the briefing's Owed + Next rows, Action Queue items naming the project, spend lines |

  Each specialty is bound to **one Kokoro voice and one stereo position for its
  lifetime**; `radio/voices.lock` records every voice ever held, and the loader
  refuses a duplicate voice, a voice outside Kokoro's English presets, a changed
  voice for a locked id, and a retired id's voice for a new one. Only an owner
  override changes a voice: an entry in the lock's `overrides` naming the id, the
  voice it replaces and the ruling that made it (`QR-n`); the replaced voice stays
  reserved. A cast edit without one fails.
- **The episode.** The host opens with what the day was, then asks each
  question in this order: what changed → is it real → what was decided → does
  a user notice → what's waiting on the owner; then signs off. The host asks the
  question itself, never the role's name; a specialty whose slice is empty is
  not asked, and any may pass ("No opinion here"). An answer cites one source
  id from its slice, says its kind aloud ("I know…", "My guess is…") and is at
  most 300 characters; host questions at most 200; the sign-off is told its
  limit. A sentence that echoes the prompt is dropped before air. The host's
  opening gives the day's shape (counts + themes), never the news. One optional
  follow-up before the Operator: the host may ask one specialty one more
  question, only if the last answer was a sourced one (GR-8). 14 calls at most,
  the last one the sign-off.
- **The Verifier** reads before it speaks: one call asks for at most 4
  read-only reads — `git show` / `log` / `grep` in the project's repos, or a
  note in its vault folders (no shell; revisions and paths validated; output
  capped) — the program runs them, and a second call gives the verdict on the
  Builder's line: "Proven: …" or "Not proven yet. The one check that settles it:
  …". An open doubt never airs. A claim found not proven is put back to its
  speaker, who confirms, corrects or withdraws; a withdrawal stays on air and the
  claim is marked withdrawn. The reads it ran are listed in the note.
- **The Operator** is off by default. `--with operator` invites it, last before
  the sign-off. It joins uninvited only when something broke in the window,
  read from the lines the project's notes gained in the window: a `BROKEN:`
  marker on a line (the reliable path), or, case-insensitively, a red gate or
  walk, a service down, a balance under its FAIL floor, a deploy rolled back. Then it says only the break and the one act
  that fixes it. The signal that fired is logged.
- The model is `claude -p --model claude-sonnet-5-5` (no tools, safe mode), run
  from `~/.cache/quorum/radio/session`, so `quorum spend --project
  ~/.cache/quorum/radio/session` meters it; `--fake` uses a deterministic
  stand-in. If the first call fails nothing is written.
- The episode is written as a note `NN - <Project> (<date or range>).md` in the
  vault's radio folder (`RADIO_DIR` in `scripts/radio.py`), recording the window,
  the vault rev it read, the cast with each specialty's status, the transcript
  (speaker / voice / line / source), the Verifier's reads, the program log, the
  call count and the spend line, and voiced to an MP3 beside it (`--no-voice`
  skips it). Play it with `afplay`.
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
scripts/radio.py           quorum radio: inputs, slices, the round, the Verifier's reads, the note (and its tests)
radio/cast.json            the cast: question, slice, never-says, bound voice + pan; radio/voices.lock
radio/projects.json        project -> briefing, folders, concept note, repos
radio-voice/               the Node voice sidecar (Kokoro + ffmpeg)
scripts/install.sh, doctor.sh
templates/skills/          the three linked skills
```
