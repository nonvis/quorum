# Quorum

Deterministic, local tooling around a Claude Code workspace. Python standard
library only; no build step. Two verbs call a model: `quorum radio` and
`quorum study` (below); everything else is $0. Radio Studio (`quorum web start`) is the one web front: a
React app over a stdlib API on 127.0.0.1.

Quorum began as a C++ daemon that orchestrated Claude Code agents with a web
dashboard. The daemon, that web app and the agent model were retired on
2026-10-16; git history keeps them. Radio Studio is not a return of either: it
has no daemon and no agents, and the owner starts and stops it.

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
quorum curate [--days N] [--show N] [--json]
quorum hook                      # Claude Code UserPromptSubmit hook (stdin JSON)
quorum hook on|off|status
quorum hook --report [--since <ISO8601>] [--json]
quorum hook --replay [--sessions N] [--show N] [--json]
quorum radio <project> [today | yesterday | week | YYYY-MM-DD | YYYY-MM-DD..YYYY-MM-DD]
             [--with operator] [--fake] [--no-voice] [--since <ISO8601>] [--until <ISO8601>]
quorum radio brainstorm <project> "<one question>" [--with <id>,…] [--context <episode id>,…]
             [--fake] [--no-voice] [--events]
quorum radio inputs <project> [range] [--since <ISO8601>] [--until <ISO8601>]
quorum web start [--fake] [--port N] [--no-open] | stop | status | logs [-f] | dev [--fake] | build
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
  dangling), its backlinks, and what it mentions outside the vault. A name several
  notes share resolves as Obsidian does: a folder written in the link
  (`[[Folder/02 - Decisions]]`), then the linker's own source (vault or memory),
  then the note nearest the linker — most leading folders shared, then its own
  folder before a subfolder; a backlink is a link that resolves to this note, not
  any link spelling its name.
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
  ambiguous ones (a bare name shared by notes equally near the linker — write the
  folder), orphan notes (no link resolves to them), live notes without `summary:`, notes naming repo files that are
  gone, memory files missing from (or index rows pointing past) the MEMORY
  indexes, repo docs that point into the vault, repo docs naming files that are
  gone, and notes whose `locked_in: <repo>/<path>` doc is missing. `--fix-safe`
  rewrites only links whose target matches exactly one note after normalising
  case, dashes and spaces. A repo's `docs/` holds what is locked and must read
  without the vault; the vault links to it (`locked_in:`), never the reverse.
- **`q`** also searches the allowlisted repos' `docs/` and root guides, listed
  after the vault hits (`--vault-only` skips them).
- **`curate`** lists the memory files no session (or subagent) Read, Edited or
  Wrote in the last N days (30), with the bytes of the `MEMORY*.md` rows that name
  them, and the total of rows naming only untouched files — in `MEMORY.md` (loaded
  by every session) and in the second-level indexes (loaded when read). Untouched
  is not stale: a memory can do its job through its one-line row. It moves
  nothing; the owner decides. The window is bounded by the transcripts on the
  host (Claude Code keeps about 30 days).
- **`hook`** is a Claude Code `UserPromptSubmit` hook. It reads the hook JSON on
  stdin and prints up to five note paths + summaries as `additionalContext`, only
  for what the operator's own words name (pasted text and harness messages do not
  count): a note as `Folder/NN` (`Notes/12`; a folder of several words also
  answers to its initials, `Daily Logs/3` as `DL/3`), and an active project from the
  vault's `06_Metadata/quorum-projects.json`, named as a whole word in the
  prompt's first 10 words (later is a passing mention) by its id, name or a
  folder's last part (folder `Partners/Acme` answers to `acme`), which brings that
  project's memory index (`MEMORY_<NAME>.md`) and its briefing notes. A note
  already injected in the session (per `hook.log`) is not repeated. Topic words
  alone inject nothing: on 530 real prompts, 2 % of word-matched notes were then
  read. It never blocks a prompt: any error exits 0 silently; `QUORUM_HOOK=off`
  disables it. Each call appends one line to `<vault>/.quorum/hook.log` (session,
  anchor count, hit paths — never the prompt). Install, in a project's
  `.claude/settings.json`: `"command": "quorum hook 2>/dev/null || true"`.
- **`hook off` / `on` / `status`** turn the hook off or on for every session in the
  vault (a flag file, `<vault>/.quorum/hook-off`); `QUORUM_HOOK=off claude` does
  it for one shell's sessions. While off, the hook injects nothing but still logs
  the call with `"mode":"off"`, so the report can compare.
- **`hook --report`** reads that log and the sessions' transcripts: prompts seen,
  injections, how many injected notes the session then Read, how many sessions
  ran a `quorum` lookup themselves, and per mode (on / off) the sessions, prompts
  and measured main-session context per turn — an A/B of the hook's cost.
  `--since <ISO8601>` (UTC) drops the log lines and lookups before it: the
  readout of a hook changed at that time.
- **`hook --replay`** puts the newest vault sessions' prompts (default 40) through
  today's matcher and scores each hit against the notes that turn (and its
  subagents) then Read; a note first committed after the prompt counts for
  neither side. It prints fired / useful prompts, precision and coverage;
  `--show N` lists the last N fired prompts with each hit marked.

## Radio

`quorum radio <project>` is a short spoken recap of **one project's day** for its
owner, run by hand (some projects get no work on a given day). The listener is
the owner, so there is no screen or trust gate.

- **The projects file** lives in the vault, `06_Metadata/quorum-projects.json`
  (so it syncs between hosts and the repo holds no vault paths). It maps each
  project to its briefing note(s), its vault folder(s), its concept note and its
  repos, plus `active` (an inactive project is refused and hidden, nothing
  deleted). Radio reads the repos on this list by path (under `~/nonvis` or
  `~/projects`), whatever the index allowlist says; a project may list none.
  Radio Studio adds, edits and (de)activates projects: each path is checked, a
  repo must be one of the owner's own `github.com/nonvis` repos (from its
  remote URL) and joins the index allowlist, and each save commits exactly the
  files it wrote, by pathspec, then pushes (a failed push is reported).
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
  | Architect | What did we decide today, and what did it cost? | the lines design / ruling notes gained in the window (another project's decision note counts when those lines name this project, with its current summary/status), the briefing's §Rulings when it changed; a range gives dated items, one per note before a second |
  | Product | Does a user notice any of it? | the diffs of user-facing files (docs, UI, CLI help), surface notes, the concept note; plus the window's merge subjects as context only (not citable) |
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
  limit. A sentence that echoes the prompt is dropped before air. A host
  question is a question only (an optional short address like "Product," and one
  interrogative sentence): a follow-up that restates a claim is re-asked once,
  then replaced by "<Specialty>, can you say more about that?". A line that calls
  something unmerged when a merge in the window names it is refused and re-asked
  once, then dropped. The host's
  opening gives the day's shape (counts + themes), never the news. One optional
  follow-up before the Operator: the host may ask one specialty one more
  question, only if the last answer was a sourced one (GR-8). 14 calls at most,
  the last one the sign-off.
- **The Verifier** reads before it speaks: one call asks for at most 4
  read-only reads — `git show` / `log` / `grep` / `branch --contains` in the
  project's repos (a grep with no match is evidence: "(no matches)"), or a
  note in its vault folders (no shell; revisions and paths validated; output
  capped) — the program runs them, and a second call gives the verdict on the
  Builder's line: "Proven: …" or "Not proven yet. The one check that settles it:
  …". An open doubt never airs. The evidence must match the kind of claim: a
  *change* (added / removed / renamed / merged) is settled by the repository; a
  *works* claim (it works / passes / is live) needs a build, test or walk record
  in a log note, and a "proven" resting on repository evidence alone is not aired. A claim found not proven is put back to its
  speaker, who confirms, corrects or withdraws; a withdrawal stays on air and the
  claim is marked withdrawn. The reads it ran are listed in the note.
- **The Operator** is off by default. `--with operator` invites it, last before
  the sign-off. It joins uninvited only when something broke in the window,
  read from the lines the project's notes gained in the window: a `BROKEN:`
  marker on a line (the reliable path), or, case-insensitively, a red gate or
  walk, a service down *with a failure signal* (a stopped or down box alone is
  routine), a balance under its FAIL floor, a deploy rolled back. A range counts
  only the markers and red gate/walk records. Then it says only the break and the one act
  that fixes it. The signal that fired is logged.
- The model is `claude -p --model claude-sonnet-5-5` (no tools, safe mode), run
  from `~/.cache/quorum/radio/session`, so `quorum spend --project
  ~/.cache/quorum/radio/session` meters it; `--fake` uses a deterministic
  stand-in. If the first call fails nothing is written.
- **The store.** Every episode goes to the per-host store
  `.quorum/radio/<id>/` in this checkout (`QUORUM_RADIO_STORE` overrides; never
  tracked): `meta.json`, `script.json` (the lines, their sources, the audio
  timings), `script.md` (the export: the window or question, the vault rev, the
  cast with each specialty's status, the script with sources, the Verifier's
  reads, the program log, the calls and the spend line) and `audio.mp3`
  (`--no-voice` skips it). The id is `YYYYMMDD-HHMMSS-<kind>-<project>` (KST). A
  new episode is unkept; an unkept one is deleted 7 days after it was made,
  swept whenever the store is listed (no timer). Keep makes it permanent; delete
  is immediate. Nothing goes to the vault: the script is exported by hand.
  `--events` prints one JSON object per line (`started`, `line`, `log`,
  `status`, `done` or `error`) for the web.
- **Brainstorm** (`quorum radio brainstorm <project> "<question>"`) is two layers.
  The **room** (`scripts/radio_room.py`) is a real deliberation: a host and the
  specialties named with `--with` (default: Builder, Verifier, Architect,
  Product), each with **its own resumable `claude -p` session** (`--session-id`
  on its first turn, `-r` after), model `claude-opus-5-5`, tools `Read,Grep,Glob`
  only (`--safe-mode`, `--strict-mcp-config`, a per-call `--max-budget-usd`),
  scoped by `--add-dir` to the project's repos and vault folders. Each specialty
  starts from its own pointers (its slice of the question's `quorum q` / `quorum
  repo q` hits and the briefing: Builder `code`, Verifier `evidence`, Architect
  `decision`, Product `surface`), reads what else it needs, cites what it read,
  labels guesses, and ends with `VERDICT:`; a later turn carries what the room
  said since it last spoke. The host routes with the HANDOFF block (`to:` a
  specialty or `wrap`, `prompt:`); the program asks everyone chosen at least
  once, sends an unknown `to` to the next unasked, wraps after two passes, and
  caps the room at 14 calls with the close (≤ 5 `THREAD:` lines) reserved. The
  **broadcast** is one `claude-sonnet-5-5` editor call that cuts the room into
  the radio script; the program keeps only lines a specialty said in the room
  (by turn), fits their length, strips spoken ids and supplies a missing
  opening or sign-off. The store keeps `room.md` (the full deliberation) beside
  `script.md`. `--context <id>,…` attaches earlier scripts. One real run: ~6
  minutes, ~$1.30. A specialty added from Radio Studio is brainstorm-only
  (`recap: false`), reads the question's general hits, and takes an unused voice
  for life (the lock records it).
- **Roles.** Each specialty (and the host) has a role file, `radio/roles/<id>.md`:
  its method, what a good answer looks like, what it never says. The text before
  the first `## Room` / `## Recap` heading goes to both kinds of episode; each
  section only to its kind. It is appended to the specialty's prompt ("Your role,
  as Sang tuned it"). The Verifier's room brief also carries the owner's ☠☠☠
  verification rules, read from his memory directory at run time, never copied
  into the repo. A specialty added from Radio Studio gets a starter role file.
- **The tuning loop.** In Radio Studio, 👍 / 👎 (with a note) on any room turn or
  broadcast line is kept with the episode (`feedback.json`). The **Roles** page
  shows each role file beside the votes against it; Save commits the file to
  this repo. **Replay** reruns one room turn with the draft role in a fresh
  session (the speaker's original brief, what the room had said before that
  turn, the host's prompt): one Opus call, before and after side by side.
- **Timings.** Every `--events` line carries `t` (seconds since the start), and
  the episode's meta keeps its phase times (`script`, or `room` + `edit`;
  `voice`; `total`).

The stdlib core never imports radio: `quorum radio` runs `scripts/radio.py` as a
subprocess, and the TTS lives in the sidecar.

## Radio Studio

`quorum web start` builds the app when its sources changed (`npm ci` when the
lockfile did), starts `scripts/radio_web.py` detached on
`http://127.0.0.1:4747` (pid + log in `.quorum/web/`) and opens the browser;
`quorum web stop` stops it; `status`, `logs [-f]`; `quorum web dev` runs Vite
with hot reload on 5173 in front of the API. `--fake` generates with the fake
model and voice. It is never a login item and never restarts itself.

- **The API** (`scripts/radio_web.py`, stdlib) serves the built app and
  `/api/…`: episodes (list, script, `script.md` export, audio with HTTP Range,
  keep, delete), generation (`radio.py … --events` as a child process, relayed
  over SSE; one at a time; cancel deletes the partial episode), projects (list,
  check, add, edit, (de)activate) and the cast (add a brainstorm specialty). It
  binds 127.0.0.1 only and refuses a foreign `Host` or `Origin` (the Vite dev
  origin is allowed).
- **The app** (`radio-web/`, Vite + React + TypeScript, Tailwind, Radix
  primitives, TanStack Query; exact pins, `package-lock.json` tracked,
  `node_modules/` and `dist/` not): **Listen** (episodes by day, filters, the
  script following the audio, keep, export, delete, "brainstorm from this"),
  **Generate** (recap or brainstorm, lines appearing as the room speaks),
  **Study** (below), **Roles** and **Projects**. One player plays across views (space, ←/→). `npm test` runs its
  unit tests.

## Study

Learn one repo about 30 minutes a day and pick it up the next day
(`scripts/study.py`; Radio Studio's **Study** tab, or `quorum study …`).

- A **track** is one repo (a git checkout under `~/nonvis` or `~/projects`), an
  optional focus subtree and a goal. It lives on one host, in
  `.quorum/study/<track>/` (`track.json` + `days/<YYYY-MM-DD>.json`); nothing
  of it is committed, and the repo is never indexed, allowlisted or written to.
- Each KST **day** is one `claude -p` session (Opus; Read, Grep and Glob over the
  repo; no Bash, so the tutor never runs git): `--session-id` on the day's first
  call, `-r` after. The tutor's role is `radio/roles/tutor.md`.
- **End** asks the same session for the close — left off, covered, still unclear,
  next — and merges it into the track's state; it also records HEAD (read from
  `.git/`) and hashes the covered files.
- The **next day** starts a new session whose first prompt carries the state, the
  last 3 closes and the covered files that changed since; never an old
  transcript. A day never ended blocks the next until it is ended.
- Nothing expires. **Finish** ends the track (an open session must be ended
  first), then offers to delete it whole.
- In the app, every `path:line` the tutor cites opens the lines beside the
  answer; the tools it ran show as it reads.

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
scripts/curate.py          quorum curate (and its tests)
scripts/radio.py           quorum radio: inputs, slices, the recap and brainstorm rounds, the Verifier's reads, the script (and its tests)
scripts/radio_room.py      the brainstorm room + its broadcast (and its tests)
scripts/radio_store.py     the per-host episode store (and its tests)
scripts/radio_projects.py  the vault's projects file: load, check, add, edit, (de)activate (and its tests)
scripts/radio_web.py       Radio Studio's API (and its tests); scripts/web_ctl.py = quorum web
scripts/study.py           quorum study: tracks, the day's tutor session, the close, the store (and its tests)
radio/cast.json            the cast: question, angle, slice, never-says, bound voice + pan; radio/voices.lock
radio/roles/<id>.md        each specialty's tuned role (method, a good answer, never; ## Room / ## Recap); tutor.md = Study's
radio-voice/               the Node voice sidecar (Kokoro + ffmpeg)
radio-web/                 Radio Studio, the React app
scripts/install.sh, doctor.sh
templates/skills/          the three linked skills
```
