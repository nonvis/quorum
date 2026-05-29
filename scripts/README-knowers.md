# Knower scripts — cartographer + architect + historian

Three read-only Quorum "knower" specialties, graduated from the bastion testbed
into reusable form. They map a workspace and answer
orientation/structure/decision questions without ever mutating the repos.

- **cartographer** — knows the project *layout* ("where is X?"). A deterministic
  Tier-1 filesystem scan + an LLM Tier-2 interpretation pass → a layout index.
- **architect** — maps how the components *interconnect* with file:line evidence,
  traces the primary flow, flags coupling/invariants.
- **historian** — knows the project's *decisions* ("what did we decide, when,
  why, what got pivoted?"). A deterministic Tier-1 mine of git commits + PRs
  (open + merged-to-main) + the Decision Log → an LLM Tier-2 interpretation
  pass → a decision-history record with provenance + supersession tracking.

All three run as `thinker` agents in `--mode brainstorm`, which clamps them to
`Read/Grep/Glob` (no Bash, no writes) so target repos are never touched.

## Scripts

### `setup-knowers.sh <project-dir>`

Scaffolds the knower setup into any project. Idempotent, spends **zero tokens**,
runs **no state-mutating git** in the target. Steps:

1. `quorum_daemon init` if `<project-dir>/.quorum/` is absent.
2. Drop `templates/knowers/CLAUDE.template.md` → `<project-dir>/CLAUDE.md`
   (only if none exists — never overwrites).
3. Refresh both Tier-1 tools — `cartographer_index.py` + `historian_mine.py` →
   `<project-dir>/.quorum/tools/` each run.
4. Create the `cartographer` + `architect` + `historian` agents (`--no-ai`,
   skipped if their yaml already exists).
5. Write `<project-dir>/.quorum/teams/knowers.yaml`
   (`default_path: [leader, cartographer, architect, historian]`).
6. Run the deterministic cartographer Tier-1 scan →
   `<project-dir>/.quorum/cartographer/layout.json`.
7. Run the deterministic historian Tier-1 mine →
   `<project-dir>/.quorum/historian/decisions-raw.json`. **This step needs an
   authenticated `gh`** (it shells out to `gh pr list` for PR data). If `gh` is
   missing or unauthenticated, the step is **skipped with a warning** (setup
   does NOT fail) — run `historian_mine.py` later once gh is ready. The tool
   also degrades gracefully on a missing git remote (empty PR lists).
8. Print a summary + next steps.

Re-running is safe: no duplicate agents, CLAUDE.md untouched, team + tools +
layout + decision record refreshed.

### `run-knower.sh <project-dir> <cartographer|architect|historian>`

Runs one **Tier-2 LLM pass** and produces the knower's vault artifact.
**This spends Claude tokens.**

Artifacts:
- cartographer → `.quorum/vaults/cartographer/knowledge/ref-project-index.md`
- architect → `.quorum/vaults/architect/knowledge/ref-architecture-map.md`
- historian → `.quorum/vaults/historian/knowledge/ref-decisions.md`

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

## Typical flow

```sh
# from the quorum repo
make build
scripts/setup-knowers.sh /path/to/workspace
#   → fill in CLAUDE.md's "## Folders" section
#   → if the historian mine was skipped (no gh auth), run it once gh is ready:
#       python3 /path/to/workspace/.quorum/tools/historian_mine.py --root /path/to/workspace
scripts/run-knower.sh /path/to/workspace cartographer   # spends tokens
scripts/run-knower.sh /path/to/workspace architect      # spends tokens
scripts/run-knower.sh /path/to/workspace historian      # spends tokens
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
- **Calibration benchmark runs.** All three knowers (cartographer / architect /
  historian) now ship the SKILL (craft) + rubric (measurement) + at least one
  synthetic benchmark. The remaining step to make each fully "Active" is running
  the calibration benchmarks (`quorum benchmark --role <knower>`) and confirming
  the scored output clears the bar.
