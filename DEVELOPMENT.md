# Quorum — Development Guide

> **Phase status / roadmap / decisions** live in the maintainer's design vault. This file is the dev-loop reference: build, test, file layout, debugging.

## Architecture

```
Orchestrator Daemon (C++20, deterministic, zero LLM in control loop)
    |
    |-- Conversation Engine (conversation mode — HANDOFF ball-passing)
    |-- Budget Enforcer (hourly caps, sequential dispatch)
    +-- Scheduler (periodic tasks)
         |
    +----+----+
    | Agents  |  <- claude -p subprocesses (all LLM here)
    +----+----+
         |
    SQLite --- task queue, conversations, agent sessions
    Vaults --- CONTEXT.md + knowledge/ per agent (filesystem)
```

## Execution

**Conversation mode only.** Old pipelines (Task Queue, Conversation analyst, Conversation executor) were replaced by the daemon's multi-agent conversation (generic/brainstorm) in Phase 2.

`quorum_daemon converse "goal"` starts a conversation. The leader agent routes work to other agents via HANDOFF blocks. Each agent responds and hands off to the next agent in the chain.

6 agent archetypes: leader, thinker, doer, scribe, librarian, evaluator. (Review/check is a thinker-role analyst specialty, not a core role.) Plus the **supervisor** — a coordination role that drives the autopilot engine (Phase 13), started interactively via `claude --agent supervisor`, not a daemon worker.

### Modes

Conversations run in one of two modes, selected per-conversation via `--mode`:

| Mode | Write surface | Use case |
|------|---------------|----------|
| `generic` (default) | Doer writes real artifacts in `target_dir` | Mutate the project |
| `brainstorm` | All agents read-only; scribe distributes cross-vault knowledge files at end of cycle | Make the team smarter for next time |

The mode is stored on the `conversations` row (`mode` column) and carried on `ConversationRecord`. In brainstorm mode, the invoker overrides every agent's class to `analyst` regardless of role — doers run read-only too.

## Agent Classes

| Class | Tools | Invoker behavior | Use case |
|-------|-------|------------------|----------|
| analyst (default) | Read-only (no Write/Edit) | `--disallowedTools "Write,Edit,NotebookEdit"` | Observation, analysis, review |
| executor | Full tool access | No `--disallowedTools`, `cd target_dir &&` prefix | Code changes, deployments |

Role determines class: doer = executor, all others = analyst.

## Build

```bash
# Build daemon + tests
make build

# Install the `quorum` CLI on PATH (~/.local/bin) + skills + supervisor agent.
# Required before using the `quorum ...` commands below or `claude --agent supervisor`.
make install

# Run daemon with verbose logging
make run-verbose

# Run tests
make test

# Start web API server + React frontend
make web-dev         # API on :3100
make web-client      # React on :3101 (proxy -> :3100)
```

### Dependencies (macOS)

```bash
brew install openssl@3 sqlite
# curl and sqlite3 provided by Xcode SDK

# Web dashboard
brew install oven-sh/bun/bun   # or: curl -fsSL https://bun.sh/install | bash
cd quorum-web && bun install && cd client && bun install
```

## CLI

```bash
# Start a conversation (creates goal + starts daemon)
quorum converse "Analyze spread performance"

# Brainstorm mode — read-only team, cross-vault knowledge distribution
quorum converse --mode brainstorm "Where should we draw module boundaries?"

# Lifecycle
quorum status                              # list conversations
quorum respond --conversation 1 "text"     # human input
quorum resume  --conversation 1            # resume a paused conversation
quorum close   --conversation 1            # close a conversation

# Project setup
quorum init                                # scaffold .quorum/ in the cwd
quorum agent create --role <r> --name <n> [--target-dir <p>] [--no-ai]
quorum agent list | modify | history

# Knowledge layer
quorum librarian curate [--dry-run|--apply]   # distill learnings → Pitch/Decision Log/Roadmap
quorum ask "<question>" [--project <p>] [--agent <name>]
quorum vault dedup [--dry-run] | vault audit [--days N]
quorum benchmark --role <r> [--task <name>]

# Autopilot engine (Phase 13) — second execution engine
quorum supervisor init [--force]           # generate ./SUPERVISOR.md + checkpoint
claude --agent supervisor                  # run the flight plan INTERACTIVELY (not claude -p)
quorum scribe record [--block <file>]      # apply a LEARNINGS_UPDATE block (output parity)
```

## Source Layout

### Web API (quorum-web/server/)

| File | Purpose |
|------|---------|
| config.ts | Paths (daemon binary, config YAML, SQLite DB) |
| server/index.ts | Hono routes — REST + SSE, CORS |
| server/db.ts | Read-only SQLite reader via bun:sqlite |
| server/daemon.ts | CLI wrapper — Bun.spawn quorum_daemon subcommands |
| server/sse.ts | SSE stream (2s poll) + auto-approve poller |

### React Frontend (quorum-web/client/src/)

| File | Purpose |
|------|---------|
| App.tsx | Root layout — SSE subscription, refresh, component wiring |
| api.ts | Fetch wrappers for all REST endpoints |
| types.ts | Conversation, Task, Stats interfaces |
| hooks/useSSE.ts | EventSource hook for real-time conversation updates |
| components/ProjectSelector.tsx | Multi-project switcher |
| components/StatsBanner.tsx | Top bar — total cost, conversation counts |
| components/PromptInput.tsx | Goal input + mode pill (generic/brainstorm) |
| components/ConversationCard.tsx | Expandable card — goal, state, tasks, cost |
| components/StateBadge.tsx | Color-coded state label |
| components/TaskTimeline.tsx | Task progression with status icons |
| components/RespondControls.tsx | Human-input box for `waiting_for_human` conversations |
| components/AgentRoster.tsx | Agent badges; click to open the CONTEXT.md editor |
| components/AgentCreateForm.tsx | Create a new agent from the browser |
| components/AgentContextEditor.tsx | Edit an agent's CONTEXT.md |
| components/ConfigPanel.tsx / BudgetPanel.tsx | Adjust config + budget window |

### Core Headers (quorum-core/src/)

| File | Purpose |
|------|---------|
| main.cpp | Entry point, CLI subcommand parse + dispatch, daemon task-dispatch loop |
| daemon/conversation.h | Conversation engine — HANDOFF ball-passing |
| daemon/scheduler.h | Periodic task scheduling |
| agent/invoker.h | claude -p subprocess, session resume, agent-class tool policy. Mode-aware: brainstorm forces analyst tools regardless of role. |
| agent/output_parser.h | HANDOFF / VAULT_UPDATE / SUMMARY / LEARNINGS_UPDATE / CURATION_UPDATE / DECISION_LOG_APPEND / EVALUATION blocks |
| agent/context_assembler.h | Prompt builder from vault files (`assemble_split` → system + user message) |
| agent/rubric.h | Rubric parser (Phase 8 quality framework) |
| vault/scribe_writer.h | `apply_scribe_learnings_update` — the canonical `.quorum/learnings.md` write (reused by autopilot for parity) |
| vault/librarian_curator.h | `apply_curation_update` / `apply_decision_log_append` — the curated-layer write |
| storage/database.h | SQLite wrapper (WAL, mutex, RAII). `ConversationRecord` carries `mode` + `no_vault_write`. |
| vault/vault_manager.h | Per-agent vault filesystem. Mode-aware cross-vault scribe writes in brainstorm. |
| cli/init.h, agent_create.h, librarian_curate.h, ask.h, supervisor_init.h, scribe_record.h, benchmark.h, vault_dedup.h, vault_audit.h | One header per `quorum <subcommand>` |
| utils/config.h | YAML config parser, AgentMetadata, load_agent_config() |
| utils/uuid.h | UUID v4 generation for session IDs |

### Tests (quorum-core/tests/)

| File | Coverage |
|------|----------|
| unit/test_output_parser.cpp | Block parsing, verdict normalization |
| unit/test_handoff_parser.cpp | HANDOFF block parsing (9 cases, 22 assertions) |
| unit/test_session_resume.cpp | UUID format, uniqueness, -r flag |
| unit/test_invoker_mode.cpp | Mode-aware tool policy (5 cases) — analyst stays analyst, executor stays executor in generic; both clamped to analyst in brainstorm. |
| integration/test_team_pipeline.cpp | Conversation pipeline + brainstorm e2e (#19 cross-vault scribe distribution, #20 read-only doer in brainstorm). |
| unit/test_scribe_write_discipline.cpp | `.quorum/learnings.md` bootstrap / append-only / canonical headers |
| unit/test_librarian_curate.cpp, test_librarian_pipeline.cpp | Curation parse → validate → diff → apply |
| unit/test_ask.cpp | `quorum ask` pure helpers (project resolve, manager-prompt assembly) |
| unit/test_supervisor_init.cpp | Autopilot `SUPERVISOR.md` generator + checkpoint skeleton (Phase 13) |
| unit/test_autopilot_parity.cpp | Output parity — autopilot scribe record == daemon, byte-identical `.quorum/learnings.md` (Phase 13 Sub-gate D) |

47 ctest targets currently passing (`cd build && ctest`).

## Design Notes

Detailed design lives in the second-brain vault:

- 01_Projects/Quorum/00 - Quorum Dashboard.md — project index
- 01_Projects/Quorum/01 - Architecture.md — architecture overview
- 01_Projects/Quorum/12 - Execution Modes.md — generic vs brainstorm; the engine axis
- 01_Projects/Quorum/99 - Quorum Manual.md — end-to-end setup + operation guide
- 01_Projects/Quorum/06 - Decision Log.md — every architectural decision

In-repo specs: `templates/specs/handoff-protocol.md` (scribe learnings), `pitch-protocol.md` (librarian curation), `autopilot-protocol.md` (Phase 13 supervisor + flight plan + checkpoint).
