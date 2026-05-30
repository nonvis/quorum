# Quorum autopilot protocol spec

> Contract for the **autopilot engine** (Phase 13): the `supervisor` agent, its
> `SUPERVISOR.md` flight plan, the `.quorum/autopilot/` checkpoint, and the
> output-parity discipline. Skill authors and the `quorum supervisor init`
> generator both implement this spec. Read it before changing either.

Spec version: 0.1
Last updated: 2026-05-30
Lineage: Research/04 - Autopilot Engine (Phase 13 design source-of-truth);
companion to `handoff-protocol.md` (scribe) and `pitch-protocol.md` (librarian).

## Why this spec exists

Autopilot is Quorum's **second execution engine**. Where the daemon spawns one
`claude -p` per HANDOFF (sequential, metered credit), autopilot is a single
**interactive** `claude --agent supervisor` session that runs an
operator-prepared flight plan by fanning out **parallel subagents** (riding the
subscription's refreshing 5h windows). It is **Model A**: no deterministic
wrapper — the LLM *is* the control loop, constrained by a generated flight plan,
a startup gate, and a checkpoint. The operator resumes on any stop.

This spec defines the three artifacts and the one discipline that make that
safe:

- **`SUPERVISOR.md`** — the generated flight-plan config the supervisor reads on
  startup (the startup gate).
- **`.quorum/autopilot/checkpoint.md`** — the resume + morning-review state the
  supervisor writes after each major task.
- **Output parity** — the rule that scribe/librarian writes go through the
  *same* code the daemon uses, so `.quorum/` accumulates byte-identically across
  engines.

## The supervisor agent

The engine is driven by exactly one agent — the **`supervisor`** — a *regular
coordination agent* (the autopilot analog of the daemon's `leader`; no rubric,
not a craft specialty). It is started **interactively** in the project
directory:

```
claude --agent supervisor        # interactive — NEVER headless `claude -p`
```

`--agent supervisor` loads `~/.claude/agents/supervisor.md`, which loads the
behavioral skill `~/.claude/skills/quorum-roles/supervisor/SKILL.md`. Install
both with `scripts/install-skills.sh`.

The supervisor is a **full-tool interactive session** (Task/Agent to fan out
subagents, Read/Bash to read the flight plan and run the `quorum` CLI, Write to
update the checkpoint). It is NOT daemon-clamped — so unlike the analyst scribe/
librarian, the supervisor writes its own checkpoint directly. It must still
route *scribe/librarian* output through the shared CLI (see Output parity).

### Startup gate

On launch the supervisor:

1. Reads `./SUPERVISOR.md` (project root). **If absent → STOP immediately** with
   a clear "not configured — run `quorum supervisor init`" message; writes
   nothing.
2. If the flight plan section is empty/placeholder → STOP the same way.
3. Otherwise reads `.quorum/autopilot/checkpoint.md` if present (resume), then
   flies the plan.

## How the supervisor is configured (three layers)

The supervisor is **not** hand-wired to specific workers — it reads its capabilities
from `SUPERVISOR.md`. Configuration is three layers, two of them automatic:

1. **The roster — auto-configured.** `quorum supervisor init` reads
   `.quorum/agents/*.yaml` and fills the `## Roster` table. Whatever agents the
   project has (created via `quorum agent create` or the web form) become
   dispatchable, for free. The supervisor equips each fanned-out subagent with
   that row's `skill` file, and dispatches **only** roster agents. To make a new
   specialty (e.g. `recap`) available: install it as a specialty, create the agent
   so it lands in `.quorum/agents/`, then **re-run `quorum supervisor init`** so it
   appears in the roster.
2. **scribe + librarian — auto-configured, but NOT via the roster.** These are the
   record-keepers, used *between* tasks (run-loop step 4), not as flight-plan
   workers. Every generated `SUPERVISOR.md` carries the fixed `## Record-keeping`
   section naming `quorum scribe record` + `quorum librarian curate`, and the
   scribe/librarian SKILLs resolve from `~/.claude/skills/quorum-roles/`. So they
   need **zero per-project setup** — the supervisor always has them.
3. **The flight plan — operator-configured.** The one hand-authored layer: which
   roster agent runs which slice (the `agent:` field per task). The generator ships
   a placeholder; the startup gate refuses to run until it is filled.

So "how does the supervisor use scribe, or recap?" — **scribe** is always wired in
(layer 2, automatic); **recap** must first exist as an agent in `.quorum/agents/`
and be picked up into the roster by re-running `init` (layer 1), then named in a
flight-plan slice (layer 3).

**Model A caveat:** these are *instructions the supervisor (an LLM) follows* from
`SUPERVISOR.md` + its SKILL — not constraints the daemon enforces at runtime
(Decision #40: determinism front-loaded into the generated config + startup gate +
checkpoint, LLM-in-the-loop accepted). The generated rails are what keep it on
track, not a wrapper.

## `SUPERVISOR.md` — file location + schema

**Location:** the **project root** (sibling of `.quorum/`), like `CLAUDE.md` /
`OPERATOR.md`. It is operator-facing config; the checkpoint (agent runtime
state) lives under `.quorum/`. `SUPERVISOR.md` is **generated** by
`quorum supervisor init`, never hand-authored (Decision #23 principle).

Canonical structure:

```markdown
---
title: Autopilot flight plan
generated_by: quorum supervisor init
spec_version: 0.1
project_root: <abs path>
---

# SUPERVISOR.md — Autopilot Flight Plan

## Project

- name: <project name>
- root: <abs path>

## Roster (subagent workers)

| agent | role | skill |
|-------|------|-------|
| <name> | <role> | <skill_file or —> |

## Record-keeping (OUTPUT PARITY — do not bypass)

- scribe → pipe each scribe LEARNINGS_UPDATE block to
  `quorum scribe record --project <root>`
- librarian → after each major task, run
  `quorum librarian curate --project <root> --apply`

## Stop conditions

- context_near_full: checkpoint + write morning review + STOP
- window_exhausted: STOP at the window edge
- needs_human: STOP, leave the question in the morning review
- max_major_tasks: <N | —>

## Flight plan

### Task 1: <title>
- agent: <roster agent>
- slices (parallel):
  - <slice prompt>
- done when: <criteria>
```

Schema rules:

- **Roster** is auto-filled from `.quorum/agents/*.yaml` at generation. The
  supervisor MAY dispatch *only* roster agents as subagents (each an existing
  specialty — no new worker agents). If empty, the generator emits
  `(no agents configured — run quorum agent create first)` and the flight plan
  is a placeholder, so the startup gate stops.
- **Record-keeping** is the parity lever (see below). It names the two CLI
  commands; the supervisor must use them rather than writing `.quorum/` itself.
- **Stop conditions** map to the checkpoint-and-halt routes.
- **Flight plan** is the operator's input: major tasks run **sequentially**;
  within a task, slices fan out as **parallel** subagents. The generator emits
  one placeholder task the operator fills/extends.

## `.quorum/autopilot/checkpoint.md` — schema

Written by the supervisor after each major task (Model A resume + morning
review). Simple markdown, no DB. Scaffolded empty by `quorum supervisor init`;
the supervisor populates the task list from `SUPERVISOR.md` on first run.

```markdown
# Autopilot checkpoint

Created at: <UTC ISO-8601>
Updated at: <UTC ISO-8601>
Flight spec: 0.1

## Major tasks

- [ ] Task 1: <title>
- [ ] Task 2: <title>

## Condensed outcomes

### Task 1
- <one-line conclusion — recorded via `quorum scribe record` at <UTC>>

## Morning review

- done: <tasks | none yet>
- pending: <tasks>
- blocked-on: <none | the human question>
```

Field rules:

- Status markers: `[ ]` pending · `[>]` in-flight · `[x]` done.
- **Condensed outcomes** hold conclusions, never raw work detail (context
  discipline — the records are the supervisor's external memory).
- `Updated at:` refreshed on every checkpoint write; `Created at:` written once.
- **Morning review** is what the operator wakes to: done / pending / blocked-on.

## Output parity (the core correctness rule)

A scribe or librarian run under autopilot MUST accumulate `.quorum/` (and the
curated aspirational layer) **byte-identically** to the daemon — or the two
engines silently drift the knowledge base. Parity is achieved by **reusing the
same write**, NOT by re-implementing it and NOT by changing the scribe:

- **scribe** — the supervisor pipes each scribe subagent's `LEARNINGS_UPDATE`
  block to `quorum scribe record --project <root>`. That CLI runs the *same*
  `parse_learnings_update()` + `apply_scribe_learnings_update()` the daemon runs
  in its task-dispatch loop (`main.cpp`), so `.quorum/learnings.md` is identical.
  The supervisor NEVER writes `.quorum/learnings.md` with its own Write tool.
- **librarian** — the supervisor runs `quorum librarian curate --project <root>
  --apply`. That is the *same* command (and the same parse→apply primitives) the
  operator runs against the daemon engine, so the curated layer
  (`Pitch/…`, `00 - Decision Log.md`, `01 - Roadmap.md`) is identical.

The existing scribe (its SKILL, `scribe_writer.h`, the daemon path) is
**unchanged**. Phase 13 ships a parity test (`test_autopilot_parity.cpp`,
Manual-Acceptance Sub-gate D) asserting the autopilot route produces
byte-identical `.quorum/learnings.md` + curated layer to the daemon route on the
same input.

## Two-level concurrency

- **Parallel *within* a major task** — the supervisor fans out the task's slices
  as parallel subagents (each equipped with a roster specialty's skill).
- **Sequential *across* major tasks** — record-keeping (scribe + librarian) runs
  between tasks, then the next task starts. This recovers Decision #13's causal
  tracing at the major-task boundary (only the intra-task fan-out is parallel).

## Context discipline

The supervisor is a **coordinator, not a doer**. It pushes heavy work into
subagents (each burns its own window) and keeps its own context lean — holding
only the flight plan, the checkpoint ledger, and **condensed** outcomes. Every
outcome is offloaded to records (`learnings.md` / curated layer) immediately, so
a *fresh* supervisor session can resume from the checkpoint + records. When
context nears full — before the hard limit — the supervisor checkpoints,
summarizes, and halts for morning resume.

## Stop → checkpoint → operator resumes (Model A)

| Stop because | What happens |
|---|---|
| context near-full | checkpoint + morning review → graceful STOP |
| 5h window exhausted | STOP at the window edge → resume after refresh |
| needs a human decision | STOP, leave the question in the morning review |
| overnight suspend | session may die → operator restarts in the morning |
| configured stop (max_major_tasks) | checkpoint + STOP |

All routes go through the checkpoint; the **operator** resumes by restarting
`claude --agent supervisor`. There is no auto-relaunch wrapper and no
TUI-puppeting (that was the rejected Model B).

## Migration policy

When this spec changes: bump the version here first; update the supervisor SKILL
and the `quorum supervisor init` generator; add a changelog entry.

## Changelog

- **0.1** (2026-05-30): Initial spec. Phase 13 autopilot engine. `SUPERVISOR.md`
  at project root + `.quorum/autopilot/checkpoint.md`; supervisor agent started
  `claude --agent supervisor` (interactive); output parity via
  `quorum scribe record` + `quorum librarian curate`; Model A operator-resume;
  parallel-within / sequential-across; context discipline. No DB, no HANDOFF;
  the existing scribe is unchanged.
