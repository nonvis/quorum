# Quorum — Development Guide

> **Phase status / roadmap / decisions** live in the maintainer's design vault. This file is the dev-loop reference: build, test, file layout, debugging.

## Architecture

```
Orchestrator Daemon (C++20, deterministic, zero LLM in control loop)
    |
    |-- Conversation Engine (conversation mode — HANDOFF ball-passing)
    |     auto-commit + phase-plan checkoff backstops on completion
    |     brainstorm staging gate (pending_vault_updates)
    |-- Budget Enforcer (window cap, sequential dispatch)
    +-- Scheduler (periodic tasks)
         |
    +----+----+
    | Agents  |  <- claude -p subprocesses (all LLM here)
    +----+----+
         |
    SQLite --- task queue, conversations, agent sessions, pending_vault_updates
    Vaults --- CONTEXT.md + knowledge/ per agent (filesystem)
```

## Execution

**Conversation mode only.** Old pipelines (Task Queue, Conversation analyst, Conversation executor) were replaced by the daemon's multi-agent conversation (generic/brainstorm) in Phase 2.

`quorum_daemon converse "goal"` starts a conversation. The leader agent routes work to other agents via HANDOFF blocks. Each agent responds and hands off to the next agent in the chain.

Core roles: **leader, thinker, doer, evaluator** (4). Plus the **4 knowers** — read-only thinker-role specialties (cartographer / architect / historian / recap = WHERE / HOW / WHY / WHAT-WHEN) that are the *sole* knowledge accumulators; each self-writes its own `.quorum/vaults/<knower>/knowledge/ref-*.md`. Plus the **supervisor** — a coordination role that drives the autopilot engine (Phase 13), started interactively via `claude --agent supervisor`, not a daemon worker. (Review/check is a thinker-role analyst specialty, not a core role.)

The default `quorum init` roster = leader + thinker + 4 knowers = **6 agents**. (Phase 14 retired the scribe and librarian roles; the scribe's one real job — auto-commit + phase-plan checkoff on completion — is now a deterministic daemon backstop in `daemon/conversation.h`.)

### Modes

Conversations run in one of two modes, selected per-conversation via `--mode`:

| Mode | Write surface | Use case |
|------|---------------|----------|
| `generic` (default) | Doer writes real artifacts in `target_dir` | Mutate the project |
| `brainstorm` | All agents read-only + human-gated; knowers self-write only their own vaults, **staged** until the human approves | Make the team smarter for next time |

The mode is stored on the `conversations` row (`mode` column) and carried on `ConversationRecord`. In brainstorm mode, the invoker overrides every agent's class to `analyst` regardless of role — doers run read-only too (no doer dispatch).

**Capture-before-gate staging (Phase 14.1c/14.1d).** A gated brainstorm does not write a knower's `VAULT_UPDATE` straight to disk. The daemon **stages** each emitted update in the `pending_vault_updates` table, prints a manifest at the gate, and waits. Only the leader ends/gates. The human commits or discards the staged writes:

- `quorum respond --conversation <id> "yes"` → COMMIT the staged writes to the knower vaults.
- `quorum respond --conversation <id> "no"` → DISCARD them (nothing lands).

Single-knower scans gate by default too; pass `--ungated` to opt a brainstorm out of the approval gate.

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

# Brainstorm mode — read-only team, human-gated staged knower writes
quorum converse --mode brainstorm "Where should we draw module boundaries?"
quorum respond --conversation <id> "yes"   # commit staged knower writes
quorum respond --conversation <id> "no"    # discard staged knower writes

# Lifecycle
quorum status                              # list conversations
quorum respond --conversation 1 "text"     # human input
quorum resume  --conversation 1            # resume a paused conversation
quorum close   --conversation 1            # close a conversation

# Project setup
quorum init                                # scaffold .quorum/ in the cwd
quorum agent create --role <r> --name <n> [--target-dir <p>] [--no-ai]
quorum agent list | modify | history

# Knowledge layer — knowers are the sole accumulators (scribe/librarian retired in Phase 14)
quorum knower refresh [--all | --knower <name>] [--project <p>]   # re-run read-only knower scan(s); knowers re-survey + self-write their vaults
quorum ask "<question>" [--project <p>] [--agent <name>]         # ask the manager (or a knower, e.g. --agent recap), read-only
quorum search "<query>" [--project <p>] [--agent <name>] [--limit N]  # deterministic $0 ranked keyword search over ref-*.md (no LLM)
quorum vault dedup [--dry-run] | vault audit [--days N]
quorum benchmark --role <r> [--task <name>]

# Autopilot engine (Phase 13) — second execution engine
quorum supervisor init [--force]           # generate ./SUPERVISOR.md + checkpoint
claude --agent supervisor                  # run the flight plan INTERACTIVELY (not claude -p)
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
| daemon/conversation.h | Conversation engine — HANDOFF ball-passing. Deterministic completion backstops: auto-commit + phase-plan checkoff (absorbed the retired scribe's one real job). Brainstorm staging gate (stage/commit/discard `pending_vault_updates`). |
| daemon/phase_plan_checkoff.h | Deterministic phase-plan checkbox flip on completion |
| daemon/scheduler.h | Periodic task scheduling |
| agent/invoker.h | claude -p subprocess, session resume, agent-class tool policy. Mode-aware: brainstorm forces analyst tools regardless of role. |
| agent/output_parser.h | HANDOFF / VAULT_UPDATE / PROPOSAL / REVIEW / OBSERVATION / SUMMARY / EVALUATION blocks |
| agent/context_assembler.h | Prompt builder from vault files (`assemble_split` → system + user message) |
| agent/rubric.h | Rubric parser (Phase 8 quality framework) |
| storage/database.h | SQLite wrapper (WAL, mutex, RAII). `ConversationRecord` carries `mode` + `no_vault_write`. Owns the `pending_vault_updates` staging table (`stage_/get_/clear_/count_pending_vault_updates`) for the brainstorm gate. |
| vault/vault_manager.h | Per-agent vault filesystem. Own-vault writes only — Phase 14 retired the brainstorm-mode scribe cross-vault exception. |
| utils/file_io.h | File read/write helpers (formerly inside the deleted scribe_writer.h; not scribe-specific) |
| cli/init.h, agent_create.h, agent_history.h, ask.h, knower_refresh.h, supervisor_init.h, benchmark.h, vault_dedup.h, vault_audit.h, skills.h | One header per `quorum <subcommand>` (knowledge layer = `ask.h` + `knower_refresh.h`; scribe/librarian headers deleted in Phase 14) |
| utils/config.h | YAML config parser, AgentMetadata, load_agent_config() |
| utils/uuid.h | UUID v4 generation for session IDs |

### Tests (quorum-core/tests/)

| File | Coverage |
|------|----------|
| unit/test_output_parser.cpp | Block parsing, verdict normalization |
| unit/test_handoff_parser.cpp | HANDOFF block parsing (9 cases, 22 assertions) |
| unit/test_session_resume.cpp | UUID format, uniqueness, -r flag |
| unit/test_invoker_mode.cpp | Mode-aware tool policy — analyst stays analyst, executor stays executor in generic; both clamped to analyst in brainstorm. |
| unit/test_generic_loop.cpp | Generic-mode ConversationEngine loop |
| integration/test_team_pipeline.cpp | Conversation pipeline + brainstorm e2e (own-vault knower writes; read-only doer in brainstorm). |
| unit/test_brainstorm_gate.cpp | Daemon-enforced brainstorm staging gate — knower VAULT_UPDATE staged (not written), human yes/no commits/discards, leader re-entry routing. |
| unit/test_brainstorm_doer_reject.cpp | Brainstorm hard-rejects a doer HANDOFF target (Decision L2). |
| unit/test_vault_update_brainstorm.cpp | VAULT_UPDATE path classification — uniform own-shape rule (Phase 14 retired the scribe cross-vault exception). |
| unit/test_no_vault_write.cpp | `--no-vault-write` suppression path end-to-end. |
| unit/test_phase_plan_checkoff.cpp | Deterministic phase-plan checkoff backstop on completion. |
| unit/test_knower_refresh.cpp | `quorum knower refresh` pure parts — name validation, `--all` order, setup checks. |
| unit/test_ask.cpp | `quorum ask` pure helpers (project resolve, manager-prompt assembly). |
| unit/test_search.cpp | `quorum search` ranked keyword scorer (filename/tag/content weights, summary preview). |
| unit/test_vault_dedup.cpp, test_vault_audit.cpp | `quorum vault dedup` / `vault audit` helpers. |
| unit/test_agent_create.cpp, test_agent_modify.cpp, test_agent_roster_dir.cpp, test_agent_hot_reload.cpp | Agent lifecycle (create / modify / roster dir / hot-reload). |
| unit/test_supervisor_init.cpp | Autopilot `SUPERVISOR.md` generator + checkpoint skeleton (Phase 13). |

The knowledge layer is verified by `test_knower_refresh.cpp`, `test_ask.cpp`, and the brainstorm-gate / vault-update tests. (Phase 14 deleted the old `test_scribe_write_discipline.cpp`, `test_librarian_curate.cpp`, `test_librarian_pipeline.cpp`, and `test_autopilot_parity.cpp` along with the scribe/librarian roles.)

46 ctest targets currently registered (`cd build && ctest`).

## Design Notes

Detailed design lives in the second-brain vault:

- 01_Projects/Quorum/00 - Quorum Dashboard.md — project index
- 01_Projects/Quorum/01 - Architecture.md — architecture overview
- 01_Projects/Quorum/12 - Execution Modes.md — generic vs brainstorm; the engine axis
- 01_Projects/Quorum/99 - Quorum Manual.md — end-to-end setup + operation guide
- 01_Projects/Quorum/06 - Decision Log.md — every architectural decision

In-repo specs: `templates/specs/autopilot-protocol.md` (Phase 13 supervisor + flight plan + checkpoint). (The old `handoff-protocol.md` / `pitch-protocol.md` specs were removed with the scribe/librarian roles in Phase 14.)
