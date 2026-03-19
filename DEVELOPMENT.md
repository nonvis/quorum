# Quorum — Development Guide

## Current Status

| Phase | Name | Status |
|-------|------|--------|
| 0 | Local Orchestration MVP | Complete (2026-03-06) |
| 0.5 | mm-bot Observation Mode | Complete (2026-03-07) |
| 0.7 | Conversation Mode | Complete (2026-03-08) |
| 1 | Executor Pipeline | Complete (2026-03-11) |
| 1b | Web Dashboard | Complete (2026-03-11) |
| 2 | Team Mode | Complete (2026-03-13) |
| 3 | Project-Local Layout | Complete (2026-03-15) |
| 4 | Web UI Management | Complete (2026-03-17) |
| 5 | Agent Quality + Templates | In Progress |

## Architecture

```
Orchestrator Daemon (C++20, deterministic, zero LLM in control loop)
    |
    |-- Conversation Engine (team mode — ball-passing via HANDOFF blocks)
    |-- Budget Enforcer (hourly caps, sequential dispatch)
    +-- Scheduler (periodic tasks)
         |
    +----+----+
    | Agents  |  <- claude -p subprocesses (all LLM here)
    +----+----+
         |
    SQLite --- task queue, conversations, knowledge ledger
    Vaults --- CONTEXT.md + knowledge/ per agent (filesystem)
```

## Execution

**Team Mode only.** Old modes (Task Queue, Conversation analyst, Conversation executor) were replaced by team mode in Phase 2.

`quorum_daemon converse "goal"` starts a conversation. The leader agent routes work to other agents via HANDOFF blocks. Each agent responds and hands off to the next agent in the chain.

6 agent archetypes: leader, thinker, doer, reviewer, scribe, librarian.

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

# List conversations
quorum status

# Respond to a conversation (human input)
quorum respond --conversation 1 "text"

# Resume a paused conversation
quorum resume --conversation 1

# Close a conversation
quorum close --conversation 1
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
| components/StatsBanner.tsx | Top bar — total cost, conversation counts |
| components/PromptInput.tsx | Goal input with auto-approve checkbox |
| components/ConversationCard.tsx | Expandable card — goal, state, tasks, cost |
| components/StateBadge.tsx | Color-coded state label |
| components/TaskTimeline.tsx | Task progression with status icons |
| components/GateControls.tsx | Approve/reject buttons for human gate |

### Core Headers (quorum-core/src/)

| File | Purpose |
|------|---------|
| main.cpp | Entry point, CLI subcommands, dispatch loop |
| daemon/conversation.h | Conversation engine — team mode ball-passing (currently stub, being rewritten in task #3) |
| daemon/scheduler.h | Periodic task scheduling |
| agent/invoker.h | claude -p subprocess, session resume, agent-class tool policy |
| agent/output_parser.h | HANDOFF/KNOWLEDGE/VAULT_UPDATE/SUMMARY blocks |
| agent/context_assembler.h | Prompt builder from vault files |
| storage/database.h | SQLite wrapper (WAL, mutex, RAII), knowledge_ledger methods |
| utils/config.h | YAML config parser, AgentMetadata, load_agent_config() |
| utils/uuid.h | UUID v4 generation for session IDs |

### Tests (quorum-core/tests/)

| File | Coverage |
|------|----------|
| unit/test_output_parser.cpp | Block parsing, verdict normalization |
| unit/test_handoff_parser.cpp | HANDOFF block parsing (9 cases, 22 assertions) |
| unit/test_knowledge_parser.cpp | KNOWLEDGE block parsing (8 cases) |
| unit/test_knowledge_ledger.cpp | Knowledge ledger DB operations (3 cases) |
| unit/test_session_resume.cpp | UUID format, uniqueness, -r flag |
| integration/test_team_pipeline.cpp | Placeholder for team mode tests |

12 ctest targets currently passing.

## Design Notes

Detailed design lives in the second-brain vault:

- 01_Projects/Quorum/00 - Quorum Dashboard.md — project index
- 01_Projects/Quorum/01 - Architecture.md — architecture overview
- 01_Projects/Quorum/11 - Conversation Mode.md — state machine design
- 01_Projects/Quorum/Phase 2/ — Phase 2 task notes and thinker prompts
