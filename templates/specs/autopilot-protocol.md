# Quorum autopilot protocol spec

> Contract for the **autopilot engine** (Phase 13): the `supervisor` agent, its
> `SUPERVISOR.md` flight plan, the `.quorum/autopilot/` checkpoint, and the
> output-parity discipline. Skill authors and the `quorum supervisor init`
> generator both implement this spec. Read it before changing either.

Spec version: 0.3
Last updated: 2026-06-03
Lineage: Research/04 - Autopilot Engine (Phase 13 design source-of-truth);
companion to `handoff-protocol.md`. (Phase 14 retired the scribe and librarian;
the knowers are the sole accumulators — see Decision #46.)

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
- **Output parity** — the rule that the supervisor's knowledge writes go through
  the *same* code the daemon uses (`quorum knower refresh`), so the knower vaults
  accumulate the way an interactive refresh would across engines. The knowers are
  the sole accumulators; there is no scribe, no librarian, and no separate
  curation step.

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
update the checkpoint). It is NOT daemon-clamped — so unlike the daemon's
read-only analysts, the supervisor writes its own checkpoint directly. It must
still route knowledge accumulation through the shared CLI (`quorum knower
refresh`, see Output parity), never by hand-editing a knower vault.

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
2. **Record-keeping — auto-configured, runs once at end-of-flight.** Knowledge
   accumulation is **not** a per-task or flight-plan-worker step. Every generated
   `SUPERVISOR.md` carries the fixed `## Record-keeping (knower refresh — end of
   flight)` section naming `quorum knower refresh`, which resolves from the
   project's installed knowers. So it needs **zero extra per-project setup** — when
   the flight completes, the supervisor refreshes the affected knower(s). There is
   no scribe, no librarian, no `learnings.md`, and no separate curation step.
3. **The flight plan — operator-configured.** The one hand-authored layer: which
   roster agent runs which slice (the `agent:` field per task). The generator ships
   a placeholder; the startup gate refuses to run until it is filled.

So "how does the supervisor accumulate knowledge, or use recap?" — knowledge is
banked by the **end-of-flight `quorum knower refresh`** (layer 2, automatic);
**recap** is one of the four knowers, refreshed by that same command (and also
available as a flight-plan worker if it exists as an agent in `.quorum/agents/`
and is picked up into the roster by re-running `init`, layer 1, then named in a
slice, layer 3).

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
spec_version: 0.3
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

## Record-keeping (knower refresh — end of flight)

- The knowers are the sole accumulators. There is no scribe and no learnings.md.
- At end-of-flight, refresh the affected knowers so their surveys re-survey the
  changed code:
  `quorum knower refresh --project <root> --all`
  (or a single lens: `--knower <cartographer|architect|historian|recap>`)

Humans read project state on demand via `quorum ask` (knower surveys + live code)
or `quorum ask --agent recap`. There is no separate curated layer to maintain.

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
- **Record-keeping** is the parity lever (see below). It names the
  `quorum knower refresh` command, which the supervisor must use at end-of-flight
  rather than hand-editing any knower vault. There is no scribe, no librarian, and
  no separate curation step.
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
- <one-line conclusion — condensed into the checkpoint at <UTC>>

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

Knowledge accumulation under autopilot MUST land in the knower vaults the same
way an interactive refresh would — or the two engines silently drift the
knowledge base. Parity is achieved by **reusing the same write**, NOT by
re-implementing it:

- **knower refresh** — at end-of-flight the supervisor runs
  `quorum knower refresh --project <root> --all` (or the affected knower(s)). Each
  pass is a read-only `converse --mode brainstorm` survey that self-writes the
  knower's `knowledge/ref-*.md` through the *same* daemon primitive an interactive
  refresh uses, so the vaults accumulate identically. The supervisor NEVER writes
  a knower vault with its own Write tool.

The knowers are the **sole accumulators** (Decision #46). There is no scribe, no
`.quorum/learnings.md`, no librarian, and no separate curation step — refreshing
the knowers at end-of-flight is the entire record-keeping path, so parity concerns
the **knower-refresh path only**.

## Two-level concurrency

- **Parallel *within* a major task** — the supervisor fans out the task's slices
  as parallel subagents (each equipped with a roster specialty's skill).
- **Sequential *across* major tasks** — the supervisor condenses each task's
  outcome into the checkpoint between tasks, then the next task starts. This
  recovers Decision #13's causal tracing at the major-task boundary (only the
  intra-task fan-out is parallel). Knowledge accumulation (knower refresh) runs
  once, at end-of-flight, not between every task.

## Context discipline

The supervisor is a **coordinator, not a doer**. It pushes heavy work into
subagents (each burns its own window) and keeps its own context lean — holding
only the flight plan, the checkpoint ledger, and **condensed** outcomes. Every
outcome is offloaded to the checkpoint immediately, so a *fresh* supervisor
session can resume from it. (Durable knowledge accumulates separately, at
end-of-flight, via `quorum knower refresh`.) When context nears full — before the
hard limit — the supervisor checkpoints, summarizes, and halts for morning resume.

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

- **0.3** (2026-06-03): Phase 14 retired the scribe and librarian. Record-keeping
  is now a single end-of-flight `quorum knower refresh` (the knowers are the sole
  accumulators, Decision #46). Output parity concerns the knower-refresh path;
  there is no `.quorum/learnings.md` and no curation step. Earlier entries below
  describe the pre-Phase-14 scribe/librarian model and are retained as history.
- **0.2** (2026-05-30): Curation removed from autopilot — `quorum librarian
  curate` is a manual, out-of-band operator action; the supervisor records scribe
  learnings only. Parity now concerns the scribe path only. The curated layer
  lives under `.quorum/librarian/` (Pitch / Decision Log / Roadmap), honed
  out-of-band whenever the operator chooses.
- **0.1** (2026-05-30): Initial spec. Phase 13 autopilot engine. `SUPERVISOR.md`
  at project root + `.quorum/autopilot/checkpoint.md`; supervisor agent started
  `claude --agent supervisor` (interactive); output parity via
  `quorum scribe record` + `quorum librarian curate`; Model A operator-resume;
  parallel-within / sequential-across; context discipline. No DB, no HANDOFF;
  the existing scribe is unchanged.
