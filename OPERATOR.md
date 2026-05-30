# Operator Manual — Quorum

Multi-agent orchestration daemon. C++20 daemon spawns `claude -p` subprocesses, manages conversations via ball-passing HANDOFF protocol, coordinates agents through filesystem vaults.

Local-first, single machine. No blockchain.

> **Phase status, roadmap, and design context** live in the maintainer's design vault. This file is a CLI cheatsheet for running Quorum day to day. For "what is it / how does it work" — see `README.md`.

## Build

```bash
# Dependencies (macOS)
brew install openssl@3 sqlite

# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run tests
cd build && ctest --output-on-failure
```

## Common Operations

### Start a Conversation (daemon engine)

```bash
# Basic — leader receives goal, team takes over (auto-discovers .quorum/)
quorum converse "Analyze mm-bot spread performance"

# Brainstorm mode — read-only team; scribe distributes cross-vault knowledge
quorum converse --mode brainstorm "Where should we draw module boundaries?"

# With budget and turn limits
quorum converse --budget 3.0 --max-rounds 5 "goal"

# Suppress vault writes for an experimental run (no knowledge pollution)
quorum converse --no-vault-write "explore X without shaping future runs"
```

`converse` exits when the conversation reaches a terminal state (the one-shot
default; pass `--keep-alive` to keep the daemon running afterward).

### Run the Autopilot Engine (Phase 13)

The second engine: a single interactive `supervisor` session runs an
operator-prepared flight plan by fanning out parallel subagents. See
`templates/specs/autopilot-protocol.md`.

```bash
# 1. Generate the flight-plan config (auto-fills the roster from .quorum/agents/)
quorum supervisor init                 # writes ./SUPERVISOR.md + .quorum/autopilot/checkpoint.md

# 2. Edit the "## Flight plan" section of SUPERVISOR.md (the startup gate stops on the placeholder)

# 3. Run it INTERACTIVELY (never headless `claude -p` — billing rides the 5h windows)
claude --agent supervisor              # cwd = the project with SUPERVISOR.md

# Record a scribe LEARNINGS_UPDATE block through the daemon's own write (output parity)
quorum scribe record --project . --block /tmp/block.txt   # or pipe on stdin
```

### Curate + Ask (knowledge layer)

```bash
# Distill accumulated scribe learnings into Pitch / Decision Log / Roadmap
quorum librarian curate                 # add --dry-run to preview, --apply to write all

# Ask a project's manager (or a specific --agent) a question, read-only
quorum ask "what did we decide about durability?" [--project <path|name>] [--agent <name>]

# Vault hygiene
quorum vault dedup [--dry-run]          # cluster near-duplicate rule-*/ref-* notes
quorum vault audit  [--days N]          # list stale (last_reviewed) + expired notes

# Calibration benchmarks for a specialty
quorum benchmark --role move-dev [--task <name>]
```

### Check Status

```bash
quorum status
```

### Respond to Leader

When the leader is waiting for human input (`waiting_for_human` state):

```bash
quorum respond --conversation 1 "response text"
```

### Resume a Paused Conversation

```bash
quorum resume --conversation 1
```

### Close a Conversation

```bash
quorum close --conversation 1
```

### Start Daemon Only

Run without a conversation subcommand (processes existing queue):

```bash
./build/quorum_daemon
```

## Web Dashboard

API server (Hono + Bun, port 3100) + React frontend (Vite + Tailwind, port 3101).

**One command (recommended)** — runs both in the background, installs deps if needed:

```bash
./scripts/web.sh start      # → dashboard at http://localhost:3101
./scripts/web.sh status     # running state
./scripts/web.sh stop       # stop both
./scripts/web.sh logs       # tail both logs
# (or: make web / make web-status / make web-stop)
```

**Foreground dev (two terminals)** — when you want live-reload output in view:

```bash
cd quorum-web && bun run dev          # Terminal 1 — API (http://localhost:3100)
cd quorum-web && bun run dev:client   # Terminal 2 — UI  (http://localhost:3101)
```

**API endpoints:**

| Method | Endpoint | Purpose |
|--------|----------|---------|
| GET | `/api/conversations` | List all conversations |
| GET | `/api/conversations/:id` | Conversation detail with tasks |
| GET | `/api/stats` | Aggregate stats |
| GET | `/api/events` | SSE stream (real-time updates) |
| POST | `/api/converse` | Start a conversation |
| POST | `/api/respond/:id` | Respond to leader |
| POST | `/api/close/:id` | Close a conversation |
| POST | `/api/resume/:id` | Resume a paused conversation |

## Data Locations

| Location | What |
|----------|------|
| `.quorum/quorum.db` | SQLite: task queue, conversations, agent sessions |
| `.quorum/vaults/{agent}/` | Agent vaults: CONTEXT.md + knowledge/ |
| `.quorum/agents/*.yaml` | Agent definitions (role, class, vault paths) |
| `.quorum/config.yaml` | Project config (daemon settings, budget, conversations) |
| `.quorum/learnings.md` | Append-only scribe learnings (handoff-protocol spec) |
| `.quorum/autopilot/checkpoint.md` | Autopilot resume + morning-review state (Phase 13) |
| `SUPERVISOR.md` (project root) | Autopilot flight plan — generated by `quorum supervisor init` |
| `.quorum/quorum.pid` | PID lock file |

## Safety Features

- **Per-task token cap**: kills `claude -p` subprocess if exceeded
- **Window budget**: daemon pauses all invocations when the window budget is exhausted (resets after `window_hours`)
- **Max turns per conversation**: set via `--max-rounds`, pauses conversation when reached
- **Sequential dispatch**: one task at a time, no concurrent agent invocations
- **PID lock**: prevents duplicate daemons on the same machine

## Health Checks

```bash
# Check if daemon is running
cat .quorum/quorum.pid && kill -0 $(cat .quorum/quorum.pid) 2>/dev/null && echo "running" || echo "stopped"

# Recent tasks (column-agnostic — avoids drift; use .headers for names)
sqlite3 -header -column .quorum/quorum.db "SELECT * FROM tasks ORDER BY rowid DESC LIMIT 10;"

# Active conversations
sqlite3 -header -column .quorum/quorum.db "SELECT * FROM conversations WHERE state != 'closed' ORDER BY rowid DESC;"

# Inspect the live schema (the source of truth is quorum-core/src/storage/schema.h)
sqlite3 .quorum/quorum.db ".schema tasks"
```

> The canonical schema lives in code at `quorum-core/src/storage/schema.h`. These
> checks use `SELECT *` / `.schema` rather than hardcoded column names so they
> never drift as the schema evolves — read the columns off the output (or the
> header) instead of memorizing them here.

## Known Issues

- `claude -p` refuses to launch inside another Claude Code session (`CLAUDECODE` env var). Must run from a regular terminal.
- Buffered stdout when redirected to file — add `std::flush` for real-time tailing.

## Environment

- **Runs on**: macOS (local, single machine)
- **Runtime dependency**: `claude` CLI must be installed and authenticated
- **DB**: `.quorum/quorum.db` (SQLite, WAL mode)
- **PID file**: `.quorum/quorum.pid`

## Troubleshooting

| Problem | Fix |
|---------|-----|
| "PID file exists" on start | `rm .quorum/quorum.pid` if process is dead |
| Stale SQLite WAL/SHM | `rm -f .quorum/quorum.db-wal .quorum/quorum.db-shm` |
| Agent invocation hangs | Check `claude` CLI auth; verify API key is valid |
| Tasks stuck in pending | Check daemon log for invoker errors; verify token budget not exhausted |
| Conversation stuck in `waiting_for_human` | Use `respond --conversation <id> "text"` to unblock |
| Window budget exhausted | Daemon pauses dispatch; increase budget via web UI or wait for window reset |
