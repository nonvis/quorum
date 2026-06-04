# Quorum

**Multi-Domain Agent Orchestration Daemon**

> Define your agents, point them at your project, let the daemon run.

A deterministic C++20 daemon that orchestrates AI agents across different project types. Same framework for trading bots, infrastructure monitoring, development workflows, or anything with ongoing operations.

## How It Works

1. **You define agents** — YAML configs + CONTEXT.md files that tell each agent its role and what to look at
2. **You seed a goal** — `quorum_daemon converse "Analyze adverse selection and propose a fix"`
3. **The daemon drives the team** — Leader receives the goal, agents pass the ball via HANDOFF blocks. One ball, always moving. Sequential dispatch, fully deterministic.
4. **Knowers accumulate knowledge** — The four read-only knowers (cartographer / architect / historian / recap) are the sole accumulators. Refresh them with `quorum knower refresh` to re-survey the codebase into their vaults, which load into future invocations. `quorum ask` answers from those knower vaults.

The daemon is 100% deterministic. No LLM in the control loop. LLMs only run in agent invocations via `claude -p` subprocesses.

## Modes

Every conversation runs in one of two modes, selected per-conversation:

| Mode | Write surface | Output | When to use |
|------|---------------|--------|-------------|
| **generic** (default) | Project files via the doer's `target_dir` | Real artifacts — code, configs, docs | You want the team to *do* the work |
| **brainstorm** | Own vault only, **human-gated** — each participating knower self-writes its own `rule-*.md`/`ref-*.md` after the operator approves | A smarter team for next time | You want the team to *think out loud* and get smarter |

In brainstorm mode, every agent — including the doer — is clamped read-only at the tool layer (the invoker overrides `agent_class` to analyst). Agents debate, plan, and reason; nothing in the project changes. Knowledge writes are **human-gated and knower-self-write**: the leader runs a read-only discussion, presents the operator a per-knower manifest of exactly what would be written where, and only on approval do the participating knowers write their *own* vault slices. No agent writes another agent's vault.

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

Both engines reuse the same specialties, rubrics, and knower-refresh knowledge primitive. Autopilot accumulates knowledge through the *same* code the daemon uses (`quorum knower refresh` at end-of-flight), so the knower vaults accumulate identically across engines. Start autopilot with `quorum supervisor init` then `claude --agent supervisor` — see `templates/specs/autopilot-protocol.md`.

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

# Brainstorm mode — read-only team, human-gated knower self-write
quorum converse --mode brainstorm "What should our caching strategy look like?"

# With custom budget and turn limit
quorum converse --max-rounds 5 "goal"

# Lifecycle
quorum status                                # list conversations
quorum respond --conversation 1 "text"       # respond when waiting_for_human
quorum resume  --conversation 1              # resume a paused conversation
quorum close   --conversation 1              # close a conversation

# Knowledge layer
quorum knower refresh --all                   # re-survey the codebase into the knower vaults
quorum ask "what did we decide about X?"      # answered from the knower vaults, read-only
quorum ask --agent recap "where did we leave off?"   # catch-up via the recap knower

# Autopilot engine (Phase 13) — overnight, parallel
quorum supervisor init                       # generate ./SUPERVISOR.md flight plan
claude --agent supervisor                    # run it interactively (NOT headless claude -p)
```

> Full setup + operation walkthrough: `01_Projects/Quorum/99 - Quorum Manual.md` in the design vault.

## Agent Archetypes

Four core roles. **Role determines tool access** — `doer` is executor (full tools); every other role is analyst (read-only). Analyst roles that "write" do so by emitting structured blocks the deterministic daemon applies — they never hold Write/Edit at runtime. (Review/check is a thinker-role analyst specialty, not a core role.) The pipeline is leader → thinker → doer → (evaluator); the doer (or evaluator, when one is in the team) is the terminal stage and hands off to `done`. There is no scribe and no librarian — the daemon persists every conversation automatically.

| Archetype | Role | Class |
|-----------|------|-------|
| **leader** | Coordinator. Receives the user goal, routes work via HANDOFF, answers read-only "ask the manager" queries. | analyst |
| **thinker** | Planner. Analyzes problems, proposes approaches, produces structured plans. | analyst |
| **doer** | Executor. Implements changes — code, config, files — in its `target_dir`. | executor |
| **evaluator** | Scorer. "Is this *good*?" Scores work against a specialty rubric; emits an `EVALUATION` block. Terminal stage — hands off to `done`. | analyst |

**Specialties** are focused (role, domain) pairings — e.g. `move-dev` or `cpp-dev` (a doer + a craft + a measured rubric), or the read-only **knowers** `cartographer` / `architect` / `historian` / `recap` (where / how / why / what-when). The four knowers are the **sole knowledge accumulators**: each self-writes its own vault during `quorum knower refresh`, and `quorum ask` answers from those vaults.

Plus one coordination role outside the daemon: the **supervisor**, which drives the autopilot engine (see Engines).

## Multi-Domain Customization

Same daemon, different agent teams. Each domain defines agent YAML configs, CONTEXT.md files, and seed knowledge.

| Domain | Example Agents | Use Case |
|--------|----------------|----------|
| Trading (mm-bot) | leader, market_thinker, bot_doer, evaluator | Optimize trading parameters, monitor P&L |
| Development | leader, arch_thinker, impl_doer, review_thinker, evaluator | Analyze codebases, propose and implement improvements |
| Infrastructure | leader, infra_thinker, ops_doer, evaluator | Monitor services, optimize resources |

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
- `POST /api/recap` — on-demand project recap (shells `quorum ask --agent recap`)

The dashboard also has a **"What's going on?" recap button** (RecapPanel) that calls `/api/recap` — on-demand catch-up via the recap knower, replacing any standing curated docs.

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
