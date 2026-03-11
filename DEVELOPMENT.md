# Quorum — Development Guide

## Current Status

| Phase | Name | Status |
|-------|------|--------|
| 0 | Local Orchestration MVP | Complete (2026-03-06) |
| 0.5 | mm-bot Observation Mode | Complete (2026-03-07) |
| 0.7 | Conversation Mode | Complete (2026-03-08) |
| 0.9 | Executor Pipeline | Complete (2026-03-11) |
| 1 | Multi-Domain Expansion | Planned |

## Architecture

```
Orchestrator Daemon (C++20, deterministic, zero LLM in control loop)
    |
    |-- Scheduler (periodic task dispatch, 5s tick)
    |-- Consensus Engine (proposal lifecycle, multi-round review)
    |-- Conversation Engine (analyst + executor pipelines, human gate)
    +-- Budget Enforcer (hourly/daily caps, sequential dispatch)
         |
    +----+----+
    | Agents  |  <- claude -p subprocesses (all LLM here)
    +----+----+
         |
    SQLite --- task queue, conversations, proposals, token tracking
    Vaults --- filesystem (CONTEXT.md + knowledge/ per agent)
```

## Execution Modes

| Mode | Trigger | Agent creates next task? |
|------|---------|------------------------|
| Task Queue | `scripts/seed_*.sh` | No — operator seeds all tasks |
| Conversation (analyst) | `quorum_daemon converse "goal"` | Yes — T->R->Done |
| Conversation (executor) | `quorum_daemon converse "goal"` | Yes — T->Gate->E->R->Done |

## Agent Classes

| Class | Tools | Invoker behavior | Use case |
|-------|-------|------------------|----------|
| analyst (default) | Read-only (no Write/Edit) | `--disallowedTools "Write,Edit,NotebookEdit"` | Observation, analysis, review |
| executor | Full tool access | No `--disallowedTools`, `cd target_dir &&` prefix | Code changes, deployments |

## Build

```bash
# Build daemon + tests
make build

# Run daemon with verbose logging
make run-verbose

# Run tests
make test
```

### Dependencies (macOS)

```bash
brew install openssl@3 sqlite
# curl and sqlite3 provided by Xcode SDK
```

## CLI

```bash
# Start a conversation (creates goal + starts daemon)
./build/quorum_daemon --config configs/quorum.yaml converse "Analyze spread performance"

# List conversations
./build/quorum_daemon --config configs/quorum.yaml status

# Resume a paused conversation
./build/quorum_daemon --config configs/quorum.yaml resume --conversation 1

# Close a conversation
./build/quorum_daemon --config configs/quorum.yaml close --conversation 1

# Approve execution at human gate
./build/quorum_daemon --config configs/quorum.yaml gate --approve --conversation 1

# Reject at human gate
./build/quorum_daemon --config configs/quorum.yaml gate --reject --conversation 1

# Start daemon only (Task Queue mode)
./build/quorum_daemon --config configs/quorum.yaml
```

## Source Layout

### Core Headers (quorum-core/src/)

| File | Purpose |
|------|---------|
| main.cpp | Entry point, CLI subcommands, dispatch loop |
| daemon/conversation.h | Conversation state machine (analyst + executor pipelines, human gate) |
| daemon/consensus.h | Proposal lifecycle, multi-round review |
| daemon/scheduler.h | Periodic task scheduling |
| agent/invoker.h | claude -p subprocess, session resume, agent-class tool policy |
| agent/output_parser.h | VAULT_UPDATE/PROPOSAL/REVIEW/OBSERVATION blocks |
| agent/context_assembler.h | Prompt builder from vault files |
| storage/database.h | SQLite wrapper (WAL, mutex, RAII) |
| utils/config.h | YAML config parser, AgentMetadata, load_agent_config() |
| utils/uuid.h | UUID v4 generation for session IDs |

### Tests (quorum-core/tests/)

| File | Coverage |
|------|----------|
| unit/test_conversation.cpp | State transitions, session reuse (27 assertions) |
| unit/test_escalation.cpp | Pause triggers, agent escalation (26 assertions) |
| unit/test_session_resume.cpp | UUID format, uniqueness, -r flag (18 assertions) |
| unit/test_output_parser.cpp | Block parsing, verdict normalization |
| integration/test_pipeline.cpp | Full Task Queue pipeline (38 assertions) |
| integration/test_conversation_pipeline.cpp | Full Conversation pipeline (34 assertions) |

## Design Notes

Detailed design lives in the second-brain vault:

- 01_Projects/Quorum/00 - Quorum Dashboard.md — project index
- 01_Projects/Quorum/01 - Architecture.md — architecture overview
- 01_Projects/Quorum/11 - Conversation Mode.md — state machine design
- 01_Projects/Quorum/12 - Execution Modes.md — mode taxonomy
