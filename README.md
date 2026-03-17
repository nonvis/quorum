# Quorum

**Multi-Domain Agent Orchestration Daemon**

> Define your agents, point them at your project, let the daemon run.

A deterministic C++20 daemon that orchestrates AI agents across different project types. Same framework for trading bots, infrastructure monitoring, development workflows, or anything with ongoing operations.

## How It Works

1. **You define agents** — YAML configs + CONTEXT.md files that tell each agent its role and what to look at
2. **You seed a goal** — `quorum_daemon converse "Analyze adverse selection and propose a fix"`
3. **The daemon drives the team** — Leader receives the goal, agents pass the ball via HANDOFF blocks. One ball, always moving. Sequential dispatch, fully deterministic.
4. **Agents accumulate knowledge** — Each turn appends to the knowledge ledger. Scribe(s) consume the ledger at the end and produce vault notes.

The daemon is 100% deterministic. No LLM in the control loop. LLMs only run in agent invocations via `claude -p` subprocesses.

## Architecture

```
Orchestrator Daemon (C++20, deterministic, zero LLM in control loop)
    |
    |-- Conversation Engine (team mode — ball-passing via HANDOFF)
    |-- Budget Enforcer (hourly caps, sequential dispatch)
    +-- Scheduler (periodic tasks — health checks, vault snapshots)
         |
    +----+----+
    | Agents  |  <- claude -p subprocesses (all LLM here)
    +----+----+
         |
    SQLite --- task queue, conversations, knowledge ledger
    Vaults --- CONTEXT.md + knowledge/ per agent (filesystem)
```

## Quick Start

```bash
# Dependencies (macOS)
brew install openssl@3 sqlite

# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Start a conversation
./build/quorum_daemon --config configs/mm-bot.yaml converse "Analyze mm-bot spread performance"

# With custom budget and turn limit
./build/quorum_daemon --config configs/mm-bot.yaml converse --budget 3.0 --max-rounds 5 "goal"

# Check status
./build/quorum_daemon --config configs/mm-bot.yaml status

# Respond to leader (when waiting for human input)
./build/quorum_daemon --config configs/mm-bot.yaml respond --conversation 1 "response text"

# Resume a paused conversation
./build/quorum_daemon --config configs/mm-bot.yaml resume --conversation 1

# Close a conversation
./build/quorum_daemon --config configs/mm-bot.yaml close --conversation 1

# Start daemon only (no conversation subcommand)
./build/quorum_daemon --config configs/mm-bot.yaml
```

## Agent Archetypes

| Archetype | Role | Tools |
|-----------|------|-------|
| **leader** | Coordinator. Receives user goal, delegates to team, synthesizes results. | Limited (planning only) |
| **thinker** | Planner. Analyzes problems, proposes approaches, produces structured plans. | Read-only |
| **doer** | Executor. Implements changes — code, config, files. Full tool access. | Full |
| **reviewer** | Validator. Reviews doer output for correctness. Optional in team. | Read-only |
| **scribe** | Knowledge to Obsidian. Consumes knowledge ledger, produces vault notes. | Write (vault only) |
| **librarian** | Knowledge to human docs. Consumes knowledge ledger, produces documentation. | Write (docs only) |

## Multi-Domain Customization

Same daemon, different agent teams. Each domain defines agent YAML configs, CONTEXT.md files, and seed knowledge.

| Domain | Example Agents | Use Case |
|--------|----------------|----------|
| Trading (mm-bot) | leader, market_thinker, bot_doer, scribe | Optimize trading parameters, monitor P&L |
| Development | leader, arch_thinker, impl_doer, reviewer, scribe | Analyze codebases, propose and implement improvements |
| Infrastructure | leader, infra_thinker, ops_doer, scribe | Monitor services, optimize resources |

## Web Dashboard

API server (Hono + Bun) and React frontend (Vite + Tailwind). Real-time updates via SSE.

```bash
# Terminal 1 — API server
cd quorum-web && bun install && bun run dev          # http://localhost:3100

# Terminal 2 — React frontend
cd quorum-web/client && bun install && bun run dev   # http://localhost:3101
```

**API endpoints:**
- `GET /api/conversations` — list all conversations
- `GET /api/conversations/:id` — conversation detail with tasks
- `GET /api/stats` — aggregate stats
- `GET /api/events` — SSE stream (real-time updates)
- `POST /api/converse` — start a conversation
- `POST /api/respond/:id` — respond to leader (when waiting for human)
- `POST /api/close/:id` / `resume/:id` — conversation lifecycle

## Project Structure

| Directory | Purpose |
|-----------|---------|
| `quorum-core/` | C++20 daemon (src/, tests/) |
| `quorum-web/` | Bun + Hono API server + React frontend (web dashboard) |
| `configs/` | Project YAML configs + agent YAMLs |
| `data/` | Runtime data — vaults, SQLite (gitignored) |
| `docs/` | Design documents |
| `.claude/commands/` | Claude Code skills |

## Status

Phase 2 — Team Mode. See [Development Guide](DEVELOPMENT.md) for details.

## License

Proprietary
