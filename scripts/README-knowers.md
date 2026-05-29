# Knower scripts — cartographer + architect

Two read-only Quorum "knower" specialties, graduated from the bastion testbed
into reusable form. They map a workspace and answer orientation/structure
questions without ever mutating the repos.

- **cartographer** — knows the project *layout* ("where is X?"). A deterministic
  Tier-1 filesystem scan + an LLM Tier-2 interpretation pass → a layout index.
- **architect** — maps how the components *interconnect* with file:line evidence,
  traces the primary flow, flags coupling/invariants.

Both run as `thinker` agents in `--mode brainstorm`, which clamps them to
`Read/Grep/Glob` (no Bash, no writes) so target repos are never touched.

## Scripts

### `setup-knowers.sh <project-dir>`

Scaffolds the knower setup into any project. Idempotent, spends **zero tokens**,
runs **no git** in the target. Steps:

1. `quorum_daemon init` if `<project-dir>/.quorum/` is absent.
2. Drop `templates/knowers/CLAUDE.template.md` → `<project-dir>/CLAUDE.md`
   (only if none exists — never overwrites).
3. Refresh `cartographer_index.py` → `<project-dir>/.quorum/tools/` each run.
4. Create the `cartographer` + `architect` agents (`--no-ai`, skipped if their
   yaml already exists).
5. Write `<project-dir>/.quorum/teams/knowers.yaml`
   (`default_path: [leader, cartographer, architect]`).
6. Run the deterministic Tier-1 scan →
   `<project-dir>/.quorum/cartographer/layout.json`.
7. Print a summary + next steps.

Re-running is safe: no duplicate agents, CLAUDE.md untouched, team + tool +
layout refreshed.

### `run-knower.sh <project-dir> <cartographer|architect>`

Runs one **Tier-2 LLM pass** and produces the knower's vault artifact.
**This spends Claude tokens.**

Artifacts:
- cartographer → `.quorum/vaults/cartographer/knowledge/ref-project-index.md`
- architect → `.quorum/vaults/architect/knowledge/ref-architecture-map.md`

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

## Typical flow

```sh
# from the quorum repo
make build
scripts/setup-knowers.sh /path/to/workspace
#   → fill in CLAUDE.md's "## Folders" section
scripts/run-knower.sh /path/to/workspace cartographer   # spends tokens
scripts/run-knower.sh /path/to/workspace architect      # spends tokens
```

## Follow-up

- ~~Fix converse self-exit in the C++ daemon.~~ **DONE (`d213496`):** `converse`
  defaults to exit-on-complete (exits when the conversation reaches a terminal
  state); `--keep-alive` opts into the old persistent-daemon behavior. The
  run-knower wrapper was simplified accordingly.
- **Knower rubrics + evaluator + benchmarks.** cartographer/architect ship the
  SKILL (craft) layer; the measurement layer (rubric + evaluator scoring +
  synthetic benchmarks) that makes them fully "Active" specialties is still TODO.
- **historian** (decisions/why) — third knower, designed but not yet built/validated.
