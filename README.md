# Quorum

**Multi-Domain Agent Orchestration Daemon**

> Define your agents, point them at your project, let the daemon run.

A deterministic C++20 daemon that orchestrates AI agents across different project types. Same framework for trading bots, infrastructure monitoring, development workflows, or anything with ongoing operations.

## How It Works

1. **You define agents** — YAML configs + CONTEXT.md files that tell each agent what to look at
2. **You seed a goal** — `quorum_daemon converse "Analyze adverse selection and propose a fix"`
3. **The daemon drives the pipeline** — Thinker proposes -> Reviewer validates -> (Executor implements)
4. **Agents accumulate knowledge** — each agent has a persistent vault (filesystem markdown)

The daemon is 100% deterministic. No LLM in the control loop. LLMs only run in agent invocations via `claude -p` subprocesses.

## Architecture

```
Orchestrator Daemon (C++20, deterministic)
    |
    |-- Conversation Engine (T/D/R state machine)
    |-- Consensus Engine (proposal lifecycle)
    |-- Scheduler (periodic dispatch)
    +-- Budget Enforcer (hourly/daily/per-conversation caps)
         |
    +----+----+
    | Agents  |  <- claude -p subprocesses (LLM here only)
    +----+----+
         |
    SQLite --- task queue, conversations, proposals
    Vaults --- CONTEXT.md + knowledge/ per agent (filesystem)
```

## Quick Start

```bash
# Dependencies (macOS)
brew install openssl@3 sqlite

# Build
make build

# Start a conversation
./build/quorum_daemon --config configs/quorum.yaml converse "Analyze mm-bot spread performance"

# Check status
./build/quorum_daemon --config configs/quorum.yaml status

# Start daemon only (Task Queue mode — seed tasks separately)
make run-verbose
```

## Multi-Domain Customization

Same daemon, different agent profiles. Each domain defines agent YAML configs, CONTEXT.md files, and seed knowledge.

| Domain | Agents | Use Case |
|--------|--------|----------|
| Trading (mm-bot) | market_analyst, bot_analyst, engineer, operator | Optimize trading parameters, monitor P&L |
| Development | product_researcher, code_quality, implementation, devops | Analyze codebases, propose improvements |
| Infrastructure | ecosystem_monitor, storage_analyst, infra_operator | Monitor services, optimize resources |

## Project Structure

| Directory | Purpose |
|-----------|---------|
| quorum-core/ | C++20 daemon (src, tests) |
| configs/ | YAML configs for daemon + agents |
| data/ | Runtime data — vaults, SQLite (gitignored) |

## Status

Phase 0.7 — Conversation Mode complete. See [Development Guide](DEVELOPMENT.md) for details.

## License

Proprietary
