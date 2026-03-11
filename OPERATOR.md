# Operator Manual — Quorum

Multi-agent orchestration framework. C++20 daemon spawns `claude -p` processes, manages task queue, coordinates agents through filesystem vaults.

Phase 0: pure local orchestration on MacBook. No blockchain, no Walrus.

## Scripts

| Script | Purpose | When to run |
|--------|---------|-------------|
| `scripts/smoke_test.sh` | End-to-end test: seed tasks → run daemon → validate results | After build, before deployment |
| `scripts/data/` | Test data and fixtures | Used by smoke_test.sh |

## Common Operations

### Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Run Daemon

```bash
./build/quorum_daemon --config configs/mm-bot.yaml
./build/quorum_daemon --config configs/mm-bot.yaml --verbose   # Verbose logging
```

Daemon creates PID lock at `/tmp/quorum.pid`. Data directory: `./data/`.

### CLI Commands

```bash
./build/quorum daemon start
./build/quorum daemon status
./build/quorum agent list
./build/quorum proposal create --title "..." --author market_analyst
./build/quorum proposal status --id proposal-042
```

### Check Status (After Overnight Runs)

```bash
./build/quorum status                  # Tasks completed, tokens spent, errors
```

### Smoke Test

```bash
# MUST be run from a regular terminal, NOT inside a claude session
./scripts/smoke_test.sh
```

Seeds tasks, runs daemon with real `claude -p` invocations, validates results. Includes WAL/SHM cleanup.

### Health Checks

```bash
# Check PID
cat /tmp/quorum.pid && kill -0 $(cat /tmp/quorum.pid) 2>/dev/null && echo "running" || echo "stopped"

# Query task queue
sqlite3 data/quorum.db "SELECT id, status, agent, created_at FROM tasks ORDER BY created_at DESC LIMIT 10;"

# Token usage
sqlite3 data/quorum.db "SELECT SUM(input_tokens), SUM(output_tokens), SUM(cost_usd) FROM task_results WHERE date(created_at) = date('now');"
```

## Data Locations

| Location | What |
|----------|------|
| `data/quorum.db` | SQLite: task queue, token tracking, proposal state |
| `data/vaults/{agent}/` | Agent vaults: CONTEXT.md, knowledge/, experiments/, decisions/, inbox/ |
| `configs/mm-bot.yaml` | Daemon config |
| `configs/agents/*.yaml` | Agent definitions |
| `/tmp/quorum.pid` | PID lock file |

## Safety Features

- **Per-task token cap**: kills `claude -p` if exceeded
- **Global daily budget**: pauses all invocations when hit
- **Max concurrent agents**: configurable (default 2-3)
- **PID lock**: prevents duplicate daemons

## Known Issues

- `claude -p` refuses to launch inside another Claude Code session (`CLAUDECODE` env var). Smoke test must run from a regular terminal.
- Daemon marks tasks `done` even on non-zero exit (if stdout is non-empty). Exit-code validation pending.
- Buffered stdout when redirected to file — add `std::flush` for real-time tailing.

## Environment

- **Runs on**: MacBook only (local)
- **Runtime dependency**: `claude` CLI must be installed and authenticated
- **DB**: `data/quorum.db` (SQLite, WAL mode)
- **PID file**: `/tmp/quorum.pid`

## Troubleshooting

| Problem | Fix |
|---------|-----|
| "PID file exists" on start | `rm /tmp/quorum.pid` if process is dead |
| Stale SQLite WAL/SHM | `rm -f data/quorum.db-wal data/quorum.db-shm` |
| Smoke test fails inside claude | Run from a regular terminal, not inside `claude` CLI |
| Agent invocation hangs | Check `claude` CLI auth; verify API key is valid |
| Tasks stuck in pending | Check daemon log for invoker errors; verify token budget not exhausted |
