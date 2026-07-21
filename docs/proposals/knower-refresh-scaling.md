# Proposal: parallelize `knower refresh --all`

**Status:** implemented behind `--parallel` — **validation gate PASSED
2026-07-21** (option 1: WAL sufficient, 5/5 clean live runs; see Validation
result below). Default stays serial as a UX choice (live streaming), not a
safety gate. · **Origin:** Crucible autopilot dogfood, 2026-07-21

## Problem

`quorum knower refresh --all` runs the four lenses **serially**
(`quorum-core/src/cli/knower_refresh.h`, `run_knower_refresh`, the loop over
`ordered_knowers()`). Each lens is a multi-minute `converse --mode brainstorm`
Tier-2 pass. `recap` (a full timeline mine) is the slowest and runs **last**, so
under any wall-clock budget on the whole command it is the first to be starved.

Observed in the Crucible autopilot flight (2026-07-21): the supervisor ran one
`--all` under a ~10-minute budget; cartographer/architect/historian completed,
then the command was SIGTERM'd mid-`recap`, leaving that lens stale. A standalone
`--knower recap` refresh also exceeded 2 minutes on a tiny greenfield repo.

## Interim mitigation (shipped)

The supervisor SKILL now refreshes **per-lens** (one command per lens, in
cartographer→architect order) instead of one `--all`, so each lens gets its own
budget and a slow lens can't starve the others. See
`templates/skills/quorum-roles/supervisor/SKILL.md` → "End-of-flight knower
refresh". This removes the observed failure without touching the daemon.

## Proposed change (daemon-side)

Parallelize the independent lenses in `run_knower_refresh`:

- **Dependency:** only cartographer→architect is stated ("architect reads the
  cartographer index"). `historian` and `recap` read their own Tier-1 inputs
  (`decisions-raw.json`, `timeline-raw.json`) and are independent.
  - *Confirm first — **RESOLVED (2026-07-21):*** the architect SKILL DOES
    opportunistically read cartographer's **Tier-2** output, not just the Tier-1
    `layout.json`. `templates/skills/architect/SKILL.md` → "Build on the
    cartographer index" (line 17, "Read those first") directs architect to read
    `.quorum/cartographer/layout.json` **AND**, when present, cartographer's
    annotated `ref-project-index.md` as its component inventory. So a fresh
    cartographer Tier-2 pass improves architect's input — the carto→arch edge is
    REAL and stays. The lenses are therefore **not** fully independent; the
    concurrency shape is three tracks: `{cartographer→architect} ∥ {historian} ∥
    {recap}` (sequential within a track), NOT a 4-way flatten.
- **Sketch:** run `{cartographer, historian, recap}` concurrently via
  `std::async(std::launch::async, run_one, lens)`; launch `architect` after
  `cartographer` resolves (or immediately, if the confirm above clears it). Tally
  per-lens exit codes; a cartographer failure skips architect but not the others.
- **Expected win:** wall time drops from `Σ(4 lenses)` to
  `max(cartographer+architect, historian, recap)` — roughly halved, more if fully
  independent.

## Implementation (2026-07-21)

Landed daemon-side in `quorum-core/src/cli/knower_refresh.h` (parsed in
`main.cpp`), unit-tested in `tests/unit/test_knower_refresh.cpp`.

- **Flag:** `--parallel` on `knower refresh` — **opt-in, `--all` only** (rejected
  with `ERROR: --parallel requires --all` before any project resolution). Default
  stays **serial**: unchanged behavior, live `std::system` streaming, stop on
  first failure.
- **Track model:** targets are grouped by `parallel_tracks()` into concurrency
  tracks — sequential *within* a track, concurrent *across* tracks. For `--all`
  this is `{cartographer→architect} ∥ {historian} ∥ {recap}` (the carto→arch
  Tier-2 edge, RESOLVED above, keeps those two in one ordered track). Each track
  runs in a `std::async(std::launch::async, …)`. A lens failure skips only the
  remaining lenses of *its own* track (cartographer failure skips architect); a
  historian failure does **not** skip recap. Exit 1 if any lens failed or was
  skipped, with a per-lens `--knower <name>` retry hint.
- **Buffered-output trade-off:** three concurrent `std::system` calls would
  interleave line-buffered output into garbage, so parallel mode instead CAPTURES
  each lens's combined stdout+stderr (via `run_command`/popen) and prints it as
  one mutex-guarded block under a `=== knower <name> finished (exit N) ===` header
  when the lens completes. stdout is in completion order, not launch order. This
  forgoes serial's live streaming — which is exactly why serial remains the
  default UX.
- **Still gated:** the ⚠️ Validation gate section below remains the blocker for
  flipping the default to parallel — `--parallel` exists so the risky
  shared-`quorum.db` (WAL) path can be validated before it becomes the default.

## ⚠️ Validation gate (do NOT merge without this)

Concurrent `converse` instances share `<project>/.quorum/quorum.db` (WAL mode —
`quorum.db-wal` present). Parallel daemons writing that db can hit SQLite lock
contention or "database is locked" errors. Before merging, validate one of:

1. **WAL is sufficient** — a real parallel `--all` run completes clean with no lock
   errors across, say, 5 repeats. (Simplest if it holds.)
2. **Per-invocation db isolation** — each lens `converse` uses its own SQLite file
   / connection, merged after. (Bigger change to `run-knower.sh` / the daemon.)
3. **Serialize only db writes** — a lightweight advisory lock around the write
   critical section, lenses otherwise parallel.

Add a `--parallel` opt-in flag first (default stays serial) so the risky path is
gated until validated, then flip the default once (1)/(2)/(3) is proven.

## ✅ Validation result (2026-07-21) — option 1 PASSED

Five consecutive `quorum knower refresh --all --parallel` runs against the live
Crucible project (`~/nonvis/crucible`, warm knowers, real Tier-2 `converse`
passes sharing one `.quorum/quorum.db` in WAL mode):

- **5/5 runs exit 0; 20/20 lens refreshes clean** — zero `database is locked` /
  `SQLITE_BUSY` / I/O errors in any captured log.
- Completion order varied across runs (recap/historian finishing before the
  cartographer→architect chain), confirming genuinely concurrent tracks.
- The project working tree and git history were untouched throughout (also a
  live confirmation of the F6 scoped auto-commit fix).

**WAL is sufficient — options (2)/(3) are unnecessary.** `--parallel` is safe to
use, including by the autopilot supervisor at end-of-flight. The **default
remains serial** as a deliberate UX choice: serial `std::system` streams each
lens's progress live to the operator's terminal, while parallel mode buffers
per-lens output to completion order. Flip the default only if/when the operator
decides buffered output is acceptable for the interactive path.

## Test impact

The pure helpers (`ordered_knowers`, `is_valid_knower`, `knower_is_setup`,
dependency order) are unchanged, so `tests/unit/test_knower_refresh.cpp` stays
green. New coverage needed: the parallel scheduler's dependency ordering + the
per-lens failure tally (a historian failure must not skip recap).
