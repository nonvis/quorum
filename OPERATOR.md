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

### Start a Conversation

```bash
# Basic — leader receives goal, team takes over (auto-discovers .quorum/)
quorum converse "Analyze mm-bot spread performance"

# With budget and turn limits
quorum converse --budget 3.0 --max-rounds 5 "goal"
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

API server (Hono + Bun, port 3100) and React frontend (Vite + Tailwind, port 3101).

```bash
# Terminal 1 — API server
cd quorum-web && bun install && bun run dev          # http://localhost:3100

# Terminal 2 — React frontend
cd quorum-web/client && bun install && bun run dev   # http://localhost:3101
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

# Query recent tasks
sqlite3 .quorum/quorum.db "SELECT id, status, agent, created_at FROM tasks ORDER BY created_at DESC LIMIT 10;"

# Token usage (today)
sqlite3 .quorum/quorum.db "SELECT SUM(input_tokens), SUM(output_tokens), SUM(cost_usd) FROM task_results WHERE date(created_at) = date('now');"

# Active conversations
sqlite3 .quorum/quorum.db "SELECT id, status, goal, created_at FROM conversations WHERE status != 'closed' ORDER BY created_at DESC;"
```

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
