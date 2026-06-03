# Quorum

**Multi-Domain Agent Orchestration Daemon**

> Define your agents, point them at your project, let the daemon run.

A deterministic C++20 daemon that orchestrates AI agents across different project types. Same framework for trading bots, infrastructure monitoring, development workflows, or anything with ongoing operations.

## How It Works

1. **You define agents** — YAML configs + CONTEXT.md files that tell each agent its role and what to look at
2. **You seed a goal** — `quorum_daemon converse "Analyze adverse selection and propose a fix"`
3. **The daemon drives the team** — Leader receives the goal, agents pass the ball via HANDOFF blocks. One ball, always moving. Sequential dispatch, fully deterministic.
4. **Agents accumulate knowledge** — At the end of each cycle, scribe(s) read the conversation transcript and produce vault notes that load into future invocations.

The daemon is 100% deterministic. No LLM in the control loop. LLMs only run in agent invocations via `claude -p` subprocesses.

## Modes

Every conversation runs in one of two modes, selected per-conversation:

| Mode | Write surface | Output | When to use |
|------|---------------|--------|-------------|
| **generic** (default) | Project files via the doer's `target_dir` | Real artifacts — code, configs, docs | You want the team to *do* the work |
| **brainstorm** | Team vaults only — scribe distributes curated knowledge files (`rule-*.md`, `ref-*.md`) across all agent vaults | A smarter team for next time | You want the team to *think out loud* and get smarter |

In brainstorm mode, every agent — including the doer — is clamped read-only at the tool layer (the invoker overrides `agent_class` to analyst). Agents debate, plan, and reason; nothing in the project changes. At the end of the cycle the scribe emits cross-vault writes: knowledge files routed to *other* agents' vaults, so the next cycle starts with a sharper team.

Sequential dispatch, HANDOFF protocol, and the agent roster are identical in both modes. Only the write surface differs.

Selection:
- CLI: `quorum converse --mode <generic|brainstorm> "goal"` (defaults to `generic`)
- Web UI: mode pill on the prompt input

## Engines

Mode is the *write-surface* axis. Orthogonal to it is the **engine** — the execution shape:

| Engine | How it runs | Dispatch | Best for |
|--------|-------------|----------|----------|
| **daemon** (default) | Deterministic daemon spawns one `claude -p` per HANDOFF | Sequential (one task at a time) | Interactive, observable, reproducible work |
| **autopilot** (Phase 13) | One interactive `supervisor` agent fans out parallel subagents on a flight plan | Parallel within a task, sequential across | Overnight, hands-off batch runs |

Both engines reuse the same specialties, rubrics, and scribe/librarian knowledge primitives. Autopilot writes `.quorum/` through the *same* code the daemon uses, so knowledge accumulates identically across engines. Start autopilot with `quorum supervisor init` then `claude --agent supervisor` — see `templates/specs/autopilot-protocol.md`.

## Architecture

```
Orchestrator Daemon (C++20, deterministic, zero LLM in control loop)
    |
    |-- Conversation Engine (conversation mode — HANDOFF ball-passing)
    |-- Budget Enforcer (hourly caps, sequential dispatch)
    +-- Scheduler (periodic tasks — health checks, vault snapshots)
         |
    +----+----+
    | Agents  |  <- claude -p subprocesses (all LLM here)
    +----+----+
         |
    SQLite --- task queue, conversations, agent sessions
    Vaults --- CONTEXT.md + knowledge/ per agent (filesystem)
```

## Quick Start

```bash
# Dependencies (macOS)
brew install openssl@3 sqlite

# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Initialize a project
cd ~/myproject && quorum init

# Create agents
quorum agent create --role leader --name leader
quorum agent create --role thinker --name architect
quorum agent create --role doer --name move-dev --target-dir .

# Start a conversation (auto-discovers .quorum/)
quorum converse "Analyze mm-bot spread performance"

# Brainstorm mode — read-only team, scribe distributes cross-vault knowledge
quorum converse --mode brainstorm "What should our caching strategy look like?"

# With custom budget and turn limit
quorum converse --budget 3.0 --max-rounds 5 "goal"

# Lifecycle
quorum status                                # list conversations
quorum respond --conversation 1 "text"       # respond when waiting_for_human
quorum resume  --conversation 1              # resume a paused conversation
quorum close   --conversation 1              # close a conversation

# Knowledge layer
quorum librarian curate                      # distill learnings → Pitch/Decision Log/Roadmap
quorum ask "what did we decide about X?"     # ask the project's manager, read-only

# Autopilot engine (Phase 13) — overnight, parallel
quorum supervisor init                       # generate ./SUPERVISOR.md flight plan
claude --agent supervisor                    # run it interactively (NOT headless claude -p)
```

> Full setup + operation walkthrough: `01_Projects/Quorum/99 - Quorum Manual.md` in the design vault.

## Agent Archetypes

Six archetypes. **Role determines tool access** — `doer` is executor (full tools); every other role is analyst (read-only). Analyst roles that "write" do so by emitting structured blocks the deterministic daemon applies — they never hold Write/Edit at runtime. (Review/check is a thinker-role analyst specialty, not a core role.)

| Archetype | Role | Class |
|-----------|------|-------|
| **leader** | Coordinator. Receives the user goal, routes work via HANDOFF, answers read-only "ask the manager" queries. | analyst |
| **thinker** | Planner. Analyzes problems, proposes approaches, produces structured plans. | analyst |
| **doer** | Executor. Implements changes — code, config, files — in its `target_dir`. | executor |
| **scribe** | Records outcomes. Emits a `LEARNINGS_UPDATE` block → daemon appends `.quorum/learnings.md`; in brainstorm, distributes cross-vault `rule-*.md`/`ref-*.md`. | analyst |
| **librarian** | Periodic curator. Distills accumulated scribe learnings into the project's Pitch / Decision Log / Roadmap via `CURATION_UPDATE` / `DECISION_LOG_APPEND` blocks behind an operator diff gate (`quorum librarian curate`). | analyst |
| **evaluator** | Scorer. "Is this *good*?" Scores work against a specialty rubric; emits an `EVALUATION` block. | analyst |

**Specialties** are focused (role, domain) pairings — e.g. `move-dev` (a doer + Move craft + a measured rubric), or the read-only "knower" thinkers `cartographer` / `architect` / `historian` (where / how / why).

Plus one coordination role outside the daemon: the **supervisor**, which drives the autopilot engine (see Engines).

## Multi-Domain Customization

Same daemon, different agent teams. Each domain defines agent YAML configs, CONTEXT.md files, and seed knowledge.

| Domain | Example Agents | Use Case |
|--------|----------------|----------|
| Trading (mm-bot) | leader, market_thinker, bot_doer, scribe | Optimize trading parameters, monitor P&L |
| Development | leader, arch_thinker, impl_doer, review_thinker, scribe | Analyze codebases, propose and implement improvements |
| Infrastructure | leader, infra_thinker, ops_doer, scribe | Monitor services, optimize resources |

## Web Dashboard

API server (Hono + Bun) and React frontend (Vite + Tailwind). Real-time updates via SSE.

```bash
# One command — runs both (API + UI) in the background, installs deps if needed
./scripts/web.sh start        # → dashboard at http://localhost:3101  (stop: ./scripts/web.sh stop)

# Or foreground dev in two terminals:
cd quorum-web && bun run dev          # Terminal 1 — API (http://localhost:3100)
cd quorum-web && bun run dev:client   # Terminal 2 — UI  (http://localhost:3101)
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
| `templates/` | Role skills, domain skills, agent CONTEXT.md templates |
| `scripts/` | install-skills.sh, lint-templates.sh, update-templates.sh |
| `docs/` | Design documents |
| `.claude/commands/` | Claude Code skills |

## Status

Active development. Phase status and roadmap live in the maintainer's private design vault. For build/dev: see [Development Guide](DEVELOPMENT.md). For ops: see [Operator Manual](OPERATOR.md).

## License

Proprietary
