# Proposal: parallelize `knower refresh --all`

**Status:** proposed (interim mitigation shipped) · **Origin:** Crucible autopilot
dogfood, 2026-07-21

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
  - *Confirm first:* the precondition (`required_input_rel`) shows architect
    depends on the **Tier-1** `cartographer/layout.json` (produced by
    `setup-knowers.sh`, always present pre-refresh), NOT on cartographer's Tier-2
    `ref-project-index.md`. If architect does not actually consume cartographer's
    Tier-2 output, **all four lenses are independent** and can run fully parallel.
- **Sketch:** run `{cartographer, historian, recap}` concurrently via
  `std::async(std::launch::async, run_one, lens)`; launch `architect` after
  `cartographer` resolves (or immediately, if the confirm above clears it). Tally
  per-lens exit codes; a cartographer failure skips architect but not the others.
- **Expected win:** wall time drops from `Σ(4 lenses)` to
  `max(cartographer+architect, historian, recap)` — roughly halved, more if fully
  independent.

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

## Test impact

The pure helpers (`ordered_knowers`, `is_valid_knower`, `knower_is_setup`,
dependency order) are unchanged, so `tests/unit/test_knower_refresh.cpp` stays
green. New coverage needed: the parallel scheduler's dependency ordering + the
per-lens failure tally (a historian failure must not skip recap).
