# Quorum — Claude Code Instructions

## Project Overview

Quorum is a **multi-agent orchestration framework**. A deterministic C++20 daemon orchestrates independent AI agents that coordinate through structured HANDOFF blocks and persist knowledge in local vaults.

**Current phase: Phase 3 — Project-Local Layout.** Pure local orchestration on a single MacBook. The daemon spawns `claude -p` (Claude Code CLI in non-interactive mode) as the agent runtime. Web3 layers (Sui, Walrus, Seal) are deferred indefinitely.

## Repo Layout

```
quorum/
├── CLAUDE.md                    <- You are here
├── configs/                     # Centralized project configs (one YAML per project)
│   ├── agents/                  # Per-project agent YAMLs
│   └── tasks/                   # Task YAML definitions
├── quorum-core/                 # C++20 (CLOSED SOURCE) — orchestrator daemon
│   ├── src/
│   │   ├── main.cpp             # Daemon entry, signal handling, PID lock, CLI subcommands
│   │   ├── daemon/              # Scheduler, message bus, events, conversation engine
│   │   ├── agent/               # Claude Code invoker, context assembler, output parser
│   │   ├── vault/               # Local vault manager (filesystem-based)
│   │   ├── storage/             # SQLite (WAL mode) — schema.h, database.h, local_cache.h
│   │   ├── utils/               # HTTP, JSON (manual), config, UUID, subprocess, discover
│   │   └── cli/                 # CLI commands (init.h, agent_create.h, skills.h)
│   └── tests/
├── quorum-web/                  # Web dashboard (Hono API + React frontend)
│   ├── config.ts                # Dynamic project config, state persistence
│   ├── server/                  # index.ts (routes), db.ts, daemon.ts (PID check), sse.ts
│   └── client/src/              # App.tsx, api.ts, types.ts, components/
├── .claude/commands/            # Claude Code skills (project scaffolding)
└── docs/                        # Design documents
```

## Reference Codebase

The bot-manager project at `../bot-manager` is the primary C++ reference. Reuse its patterns for HTTP, JSON, crypto, SQLite, and general C++ style.

## Hard Constraints

### C++ Conventions

- **C++20 standard** — concepts, ranges, designated initializers, std::format
- **Namespace:** `sui::quorum`
- **No nlohmann/json** — use manual `json.h` parsing (see bot-manager)
- **No Boost** — ever
- **No exceptions for control flow** — use return codes / std::optional / std::expected
- **Header-only preferred** for new modules
- **Timestamps:** epoch seconds as `uint64_t`
- **Logging:** stdout/stderr only
- **String handling:** `std::string_view` for read-only, `std::string` for ownership
- **Error handling:** return `bool` or `std::optional<T>` — never throw

### Dependencies

| Dependency | Purpose |
|-----------|---------|
| sqlite3 | Task queue, token tracking, conversations, knowledge, budget |
| OpenSSL | Hashing (SHA-256), TLS |
| libcurl | HTTP client |

**Runtime dependency:** `claude` CLI (Claude Code) must be installed and authenticated.

**Banned:** nlohmann/json, Boost, any JSON library, any ORM, any framework

### File Naming

- Headers: `snake_case.h` — Sources: `snake_case.cpp` — Tests: `test_<module>.cpp` — Configs: `snake_case.yaml`

## Architecture Rules

### The Daemon Is Deterministic

The daemon **never** calls an LLM. All scheduling, routing, and event handling is pure C++ logic. Agent invocations happen by spawning `claude -p` subprocesses, triggered by the task dispatch loop.

### Agent Invocation

```
daemon -> assembles prompt (vault + task) -> spawns `claude -p` with class-appropriate flags -> parses result -> writes to vault -> routes follow-up tasks
```

**Agent classes** (set in `AgentMetadata`, configured via `role` in agent YAML):
- **`analyst`** (default, all roles except doer): `--disallowedTools "Write,Edit,NotebookEdit"` — read-only
- **`executor`** (doer role): full tool access, `target_dir` sets working directory

**Session management** (conversation mode only):
- First use: `--session-id <uuid>` — Subsequent: `-r <uuid>` (resume)
- Resume fallback: if `-r` fails, retries with `--session-id` (fresh session)
- Task Queue mode (no `conversation_id`): no session flag, fresh context each time

### Agent Output Rules (Defense-in-Depth)

Analysts must produce structured blocks (VAULT_UPDATE, SUMMARY, HANDOFF, KNOWLEDGE) — never write files directly. Enforced at three layers:
1. **`--disallowedTools`** — hard enforcement for analyst agents
2. **CONTEXT.md** — per-agent instructions prohibiting file writes
3. **Context assembler** — injected into every prompt as failsafe

Executors are exempt — they write files directly in their `target_dir`.

### Sequential Dispatch

One `claude -p` task at a time, always. Design decision, not configuration.
- Per-task token cap (kills process if exceeded) + window budget (pauses dispatch when exhausted)
- Crash recovery: `recover_stale_tasks()` on startup marks stuck `active` tasks as `failed`, re-dispatches affected conversations to leader

### Vault System

Each agent owns a vault: `{data_dir}/vaults/{agent_name}/`
- `CONTEXT.md` — agent identity + instructions (auto-generated, never hand-edit)
- `knowledge/` — accumulated findings via VAULT_UPDATE blocks

### Knowledge System

- **KNOWLEDGE blocks** — ephemeral per-conversation, stored in `knowledge_ledger` SQLite table
- **VAULT_UPDATE blocks** — persistent per-agent, written to `knowledge/` directory

### Conversation Mode (Team Mode)

`ConversationEngine` (`daemon/conversation.h`) coordinates agent teams via HANDOFF blocks.

**States:** `active`, `waiting_for_human`, `done`, `closed`, `paused`

**Key types:**
- `ConversationRecord` — in `storage/database.h` (id, goal, state, round, max_rounds, budget_usd, spent_usd, current_agent, path_index, team)
- `TeamPreset` — in `utils/config.h` (id, name, default_path)
- `ConversationEngine` — header-only in `daemon/conversation.h`. Methods: `start()`, `on_task_complete()`, `respond()`, `resume()`, `close()`, `recover()`

**Database tables:** `conversations`, `tasks` (with conversation_id, session_id), `knowledge_ledger`, `budget_window` (singleton, tracks window spend with auto-reset)

## Key Patterns

### JSON (manual — no libraries)
```cpp
auto text = sui::quorum::json::extract_string(json_str, "result");
auto cost = sui::quorum::json::extract_number(json_str, "total_cost_usd");
auto toks = sui::quorum::json::extract_int(json_str, "input_tokens");
auto flag = sui::quorum::json::extract_bool(json_str, "is_error");
```

### SQLite
```cpp
sui::quorum::Database db("quorum.db");  // WAL mode, mutex-protected, RAII
db.execute("CREATE TABLE IF NOT EXISTS metrics (...)");
auto result = db.query("SELECT * FROM metrics WHERE agent = ?", agent_id);
```

### Config (project-local — preferred)
```yaml
# .quorum/config.yaml — agents auto-loaded from .quorum/agents/
daemon:
  data_dir: .quorum
  pid_file: .quorum/quorum.pid
budget:
  window_budget_usd: 100.00
  window_hours: 5
conversations:
  enabled: true
  default_max_rounds: 20
  default_budget_usd: 5.0
  leader: leader
```

### Agent YAML
```yaml
# Analyst (read-only tools)
id: market_analyst
name: "Market Analyst"
role: thinker                    # leader, thinker, doer, reviewer, scribe, librarian
agent_class: analyst             # auto-derived from role (doer=executor, all others=analyst)
description: "Analyzes market structure"
vault_path: data/vaults/market_analyst/
context_file: data/vaults/market_analyst/CONTEXT.md
skill_file: data/vaults/market_analyst/SKILL.md   # optional
```
```yaml
# Executor (full tool access)
id: move-dev
name: "Move Developer"
role: doer
agent_class: executor
description: "Writes Move smart contracts"
vault_path: data/vaults/move-dev/
context_file: data/vaults/move-dev/CONTEXT.md
executor:
  target_dir: ~/nonvis/my-project   # supports ~/ expansion
```

### Project-Local Layout (`.quorum/`)

`quorum init` creates a self-contained `.quorum/` directory with a ready-to-use SQLite database:
```
myproject/.quorum/
  config.yaml, .gitignore, quorum.db (schema pre-created), quorum.pid
  agents/     — agent YAML configs (auto-discovered)
  vaults/     — per-agent vaults (CONTEXT.md + knowledge/)
  teams/      — team presets (named default_path configs)
```

**Schema management:** `storage/schema.h` defines `create_schema()` — all CREATE TABLE/INDEX statements in one place. Called by both `quorum init` (so DB is usable immediately) and `init_schema()` in main.cpp (which additionally runs ALTER TABLE migrations for old databases).

**Team presets** (`teams/*.yaml`): `name` + `default_path` array. Selected via `--team <name>`. Overrides `conversations.default_path` for that conversation. All agents remain available for HANDOFF regardless of team.

**Auto-discovery:** Without `--config`, the daemon walks up from cwd for `.quorum/config.yaml`, then `chdir`s to project root. All CLI commands work without flags from anywhere inside a `.quorum/` project.

**Two layouts coexist:** Project-local (`.quorum/`, preferred) and centralized (`configs/` + `data/`, requires `--config`).

**Config loading:** If `agents/` dir exists next to config, `load_agents_from_directory()` scans for `.yaml`/`.yml` files sorted by id, replacing any explicit `agents:` list.

**Web UI stale detection:** `GET /api/daemon/status` returns `{ running: boolean }` (checks PID file + `process.kill(pid, 0)`). When daemon is not running and active conversations exist, the UI shows an amber warning banner advising the user to run `quorum status` to trigger crash recovery.

## What NOT To Do

1. **Never add LLM calls to the daemon** — the orchestrator is deterministic
2. **Never use nlohmann/json or any JSON library** — manual json.h only
3. **Never use Boost**
4. **Never block the daemon event loop**
5. **Never run `claude -p` without budget enforcement**
6. **Never let analyst agents write files** — enforced via `--disallowedTools`

## Known Issues

- **Nested Claude Code sessions:** `claude -p` refuses to launch inside another Claude Code session (`CLAUDECODE` env var detected).
- **Buffered stdout in background mode:** When stdout is redirected, `std::cout` uses full buffering. Add `std::flush` for real-time log tailing.

## Useful Commands

```bash
# Build
cmake -B build && cmake --build build -j$(nproc)

# Tests
cd build && ctest --output-on-failure

# Initialize project
cd ~/myproject && quorum init

# Agent management (auto-discovers .quorum/)
quorum agent create --role doer --name my-dev --target-dir .
quorum agent create --role doer --name my-dev --target-dir . --skill move-developer
quorum agent modify --name my-dev --role thinker
quorum agent modify --name my-dev --skill move-developer
quorum agent list
quorum skills
quorum teams

# Run daemon
./build/quorum_daemon                                          # auto-discover .quorum/
./build/quorum_daemon --config configs/mm-bot.yaml --verbose   # explicit config

# Conversations
quorum converse "Build a REST API"
quorum converse --team quick-build "fix the login bug"
quorum status
./build/quorum_daemon --config configs/mm-bot.yaml resume --conversation 1
./build/quorum_daemon --config configs/mm-bot.yaml close --conversation 1

# What the daemon spawns (for reference)
claude -p "prompt" --dangerously-skip-permissions --disallowedTools "Write,Edit,NotebookEdit" --output-format json  # analyst
cd ~/project && claude -p "prompt" --dangerously-skip-permissions --output-format json                              # executor
```
