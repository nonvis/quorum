# Knower scripts — cartographer + architect + historian + recap

Four read-only Quorum "knower" specialties, graduated from the bastion testbed
into reusable form. They map a workspace and answer
orientation/structure/decision/activity questions without ever mutating the repos.

- **cartographer** — knows the project *layout* ("where is X?"). A deterministic
  Tier-1 filesystem scan + an LLM Tier-2 interpretation pass → a layout index.
- **architect** — maps how the components *interconnect* with file:line evidence,
  traces the primary flow, flags coupling/invariants.
- **historian** — knows the project's *decisions* ("what did we decide, when,
  why, what got pivoted?"). A deterministic Tier-1 mine of git commits + PRs
  (open + merged-to-main) + the Decision Log → an LLM Tier-2 interpretation
  pass → a decision-history record with provenance + supersession tracking.
- **recap** — knows *what changed and when* ("catch me up on `<project>` since I
  last looked, where did I leave off?"). A deterministic Tier-1 **windowed** mine
  of git commits (with changed files) + merged/open PRs + where-I-left-off facts
  → an LLM Tier-2 pass weaving one dated, component-grouped timeline + a
  where-I-left-off draft, with an optional untimed Linear status overlay. The
  WHAT/WHEN sibling of the historian's WHY/all-time — distinct file + schema.
  **First knower with no rubric/evaluator** — by design (bar is
  condensed/focused/never-deep + don't-fabricate; git is the check).

All four run as `thinker` agents in `--mode brainstorm`, which clamps them to
`Read/Grep/Glob` (no Bash, no writes) so target repos are never touched.

## Scripts

### `setup-knowers.sh <project-dir>`

Scaffolds the knower setup into any project. Idempotent, spends **zero tokens**,
runs **no state-mutating git** in the target. Steps:

1. `quorum_daemon init` if `<project-dir>/.quorum/` is absent.
2. Drop `templates/knowers/CLAUDE.template.md` → `<project-dir>/CLAUDE.md`
   (only if none exists — never overwrites).
3. Refresh all three Tier-1 tools — `cartographer_index.py` + `historian_mine.py`
   + `recap_mine.py` → `<project-dir>/.quorum/tools/` each run.
4. Create the `cartographer` + `architect` + `historian` + `recap` agents
   (`--no-ai`, skipped if their yaml already exists).
5. Write `<project-dir>/.quorum/teams/knowers.yaml`
   (`default_path: [leader, cartographer, architect, historian, recap]`).
6. Run the deterministic cartographer Tier-1 scan →
   `<project-dir>/.quorum/cartographer/layout.json`.
7. Run the deterministic historian Tier-1 mine →
   `<project-dir>/.quorum/historian/decisions-raw.json`. **This step needs an
   authenticated `gh`** (it shells out to `gh pr list` for PR data). If `gh` is
   missing or unauthenticated, the step is **skipped with a warning** (setup
   does NOT fail) — run `historian_mine.py` later once gh is ready. The tool
   also degrades gracefully on a missing git remote (empty PR lists).
8. Run the deterministic recap Tier-1 windowed mine →
   `<project-dir>/.quorum/recap/timeline-raw.json`, then seed the operator-owned
   dump channels `messages-dump.md` + `linear-dump.md` (stubs, **only if absent**
   — never overwrites an existing dump). **Unlike step 7, this runs
   unconditionally** — the git timeline is always emitted; `gh` only *enriches*
   it with PR data, so a missing/unauthenticated `gh` just yields empty PR lists.
9. Print a summary + next steps.

Re-running is safe: no duplicate agents, CLAUDE.md untouched, team + tools +
layout + decision record refreshed.

### `run-knower.sh <project-dir> <cartographer|architect|historian|recap>`

Runs one **Tier-2 LLM pass** and produces the knower's vault artifact.
**This spends Claude tokens.**

Artifacts:
- cartographer → `.quorum/vaults/cartographer/knowledge/ref-project-index.md`
- architect → `.quorum/vaults/architect/knowledge/ref-architecture-map.md`
- historian → `.quorum/vaults/historian/knowledge/ref-decisions.md`
- recap → `.quorum/vaults/recap/knowledge/ref-recap.md`

(recap's `ref-recap.md` carries the dated timeline + the where-I-left-off
*draft*. The operator owns the intended "next step" line and promotes the
finalized marker to the operator-owned `.quorum/recap/where-i-left-off.md` — the
brainstorm clamp + the own-vault `VAULT_UPDATE` rule mean recap can only write
under its own `knowledge/`, never the `.quorum/recap/` dump dir.)

`quorum converse` now **exits cleanly when its conversation reaches `done`**
(default behavior as of `d213496`; pass `--keep-alive` for a persistent daemon).
So this wrapper just runs converse and lets it self-exit, then prints whether
the artifact is PRESENT. A background-launch + wait-for-exit + last-resort kill
remains only as a safety net (warns if it ever fires); it is no longer the
primary mechanism.

### `cartographer_index.py`

The deterministic Tier-1 indexer (read-only, no LLM). Scans a workspace's
top-level folders and captures per-component: detected languages (manifests +
source-extension counts), git branch + tracked/dirty counts, README headline,
subfolders, key subdirs, and CLAUDE.md locations. Emits
`.quorum/cartographer/layout.json`. Cheap enough to run many times a day; the
LLM cartographer (Tier 2) reads this + the CLAUDE.md(s) and annotates
purpose/importance.

```
python3 .quorum/tools/cartographer_index.py --root <dir>
```

`setup-knowers.sh` copies this into each project's `.quorum/tools/` and runs it.

### `historian_mine.py`

The deterministic Tier-1 decision miner (read-only, no LLM). Per top-level git
repo, captures recent **commits** (hash/date/**author**/subject), **open PRs**,
and **PRs merged to the default branch (`main`)** (number/title/author/mergedAt/
url), and notes a project Decision Log if present. Emits
`.quorum/historian/decisions-raw.json`. The LLM historian (Tier 2) reads this +
the CLAUDE.md(s), recognizes significant decisions (merged-to-main PRs are
first-class), and tracks pivots/supersession with provenance.

**Needs an authenticated `gh`** for the PR data (`gh pr list`); it degrades
gracefully (empty PR lists) when a git remote is absent. Uses only read-only
git + `gh` + file reads — never mutates a repo.

```
python3 .quorum/tools/historian_mine.py --root <dir>
```

`setup-knowers.sh` copies this into each project's `.quorum/tools/` and runs it
(skipping the run with a warning if `gh` is unauthenticated).

### `recap_mine.py`

The deterministic Tier-1 **windowed** miner (read-only, no LLM) for the recap
knower. Per top-level git repo, over a window (`--since`, default `"1 month
ago"`), captures: **windowed commits with their changed files** (`git log
--name-only`), **PRs merged within the window** + open PRs, and the
**where-I-left-off mechanical facts** git can show (current branch, last commit,
dirty/ahead/stash counts). Emits a DATED windowed timeline to
`.quorum/recap/timeline-raw.json` (`schema: recap/tier1-timeline/v1`). The LLM
recap (Tier 2) reads this + the operator dumps and weaves one component-grouped
timeline + a where-I-left-off draft.

**Distinct from `historian_mine.py`** — historian = WHY / all-time decision
record (`decisions-raw.json`); recap = WHAT-WHEN / windowed dated timeline
(`timeline-raw.json`). Different file, different schema, different vault.

The **git timeline is always emitted**; `gh` only *enriches* it with merged/open
PR data (degrades gracefully to empty PR lists when `gh`/auth/remote is absent —
so `setup-knowers.sh` runs it unconditionally, unlike the gh-gated historian
step). The window anchor (`window_start_date`, used to filter merged PRs) is
derived dependency-free: the min date across all in-window commits, falling back
to today on an empty window.

```
python3 .quorum/tools/recap_mine.py --root <dir> [--since "<range>"]
```

A standalone deterministic check — `scripts/recap_mine_test.py` (stdlib-only,
builds a throwaway git repo straddling the window and asserts the right commits
are captured / out-of-window excluded / mechanical facts correct) — pins the
windowing behavior; run `python3 scripts/recap_mine_test.py` (exit 0 = pass).
recap ships **SKILL + this Tier-1 check, no scored rubric/evaluator** by design.

### `recap_messages_import.py`

The deterministic Slack-paste → `messages-dump.md` formatter (read-only, no LLM).
The operator **curates** which messages are relevant (recap is condensed — don't
hand it the whole channel); this tool only **formats + dates** them, the same way
every run (no model in the loop, so no run-to-run drift). Dating is deterministic:
it reads Slack's **day-divider** lines (`Monday, May 25th` / `May 25th` /
`2026-05-25` / `Today`+`--today`) and carries the date down to each message;
clock times come from the `Author  [H:MM AM]` stamp.

```
python3 scripts/recap_messages_import.py --in paste.txt \
    --out .quorum/recap/messages-dump.md --channel <name> [--year YYYY]
```

**Input caveat (the copy-paste trap):** a raw Slack copy DROPS the day dividers,
leaving only clock times across a multi-week thread — undatable. Re-select so the
dividers come along, or use a Slack JSON export. Messages before the first divider
are skipped with a warning (no anchor) unless `--start-date` is given. Appends with
dedup (skips a block whose stamp line already exists). `--selftest` runs the
built-in fixture (exit 0 = pass).

## Typical flow

```sh
# from the quorum repo
make build
scripts/setup-knowers.sh /path/to/workspace
#   → fill in CLAUDE.md's "## Folders" section
#   → if the historian mine was skipped (no gh auth), run it once gh is ready:
#       python3 /path/to/workspace/.quorum/tools/historian_mine.py --root /path/to/workspace
#   → recap reads two operator-owned dumps (seeded as stubs if absent):
#       .quorum/recap/messages-dump.md  (timestamped chat → timeline)
#       .quorum/recap/linear-dump.md    (Linear export → untimed overlay)
scripts/run-knower.sh /path/to/workspace cartographer   # spends tokens
scripts/run-knower.sh /path/to/workspace architect      # spends tokens
scripts/run-knower.sh /path/to/workspace historian      # spends tokens
scripts/run-knower.sh /path/to/workspace recap          # spends tokens
```

## Follow-up

- ~~Fix converse self-exit in the C++ daemon.~~ **DONE (`d213496`):** `converse`
  defaults to exit-on-complete (exits when the conversation reaches a terminal
  state); `--keep-alive` opts into the old persistent-daemon behavior. The
  run-knower wrapper was simplified accordingly.
- **historian** (decisions/why) — third knower, now graduated to reusable form:
  Tier-1 miner (`historian_mine.py`), generalized SKILL
  (`templates/skills/historian/`), rubric (`templates/rubrics/historian/`,
  categories sum to 100), and an offline benchmark
  (`templates/benchmarks/historian/historian-pivot/`, a pre-baked Tier-1 record
  so no `gh`/network is needed). Validated end-to-end on the bastion testbed.
- **Calibration benchmark runs.** Three knowers (cartographer / architect /
  historian) ship the SKILL (craft) + rubric (measurement) + at least one
  synthetic benchmark, and are calibrated via `quorum benchmark --role <knower>`
  (all 100.0). **recap is the deliberate exception** — it ships SKILL + a
  deterministic Tier-1 check (`recap_mine_test.py`) but **no rubric and no
  evaluator** (the first knower without one); its bar is qualitative
  (condensed/focused/never-deep + don't-fabricate) and validated by a manual
  eyeball gate, not a scored benchmark.
- **recap** (activity / WHAT-WHEN) — fourth knower, graduated to reusable form:
  windowed Tier-1 miner (`recap_mine.py` + `recap_mine_test.py`), generalized
  SKILL (`templates/skills/recap/`), `setup-knowers.sh` + `run-knower.sh` wiring,
  and operator-owned dump channels (`messages-dump.md` / `linear-dump.md`).
  Validated end-to-end on the bastion testbed.
