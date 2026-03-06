# Quorum — Claude Code Instructions

## Project Overview

Quorum is a **multi-agent orchestration framework**. A deterministic C++20 daemon orchestrates independent AI agents that coordinate through structured proposals and persist knowledge in local vaults.

**Current phase: Phase 0 — Pure local orchestration.** No blockchain, no Walrus, no Seal. Everything runs on a single MacBook. The daemon spawns `claude -p` (Claude Code CLI in non-interactive mode) as the agent runtime. Web3 layers (Sui, Walrus, Seal) will be added in later phases after the core orchestration loop is proven.

**Tagline:** "Your AI agents now have verifiable memory and auditable decisions."

## Repo Layout

```
quorum/
├── CLAUDE.md                    ← You are here
├── quorum-core/                 # C++20 (CLOSED SOURCE) — orchestrator daemon
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── main.cpp             # Daemon entry, signal handling, PID lock
│   │   ├── daemon/              # Scheduler, router, message bus, events
│   │   ├── agent/               # Claude Code invoker (claude -p), context assembler, output parser
│   │   ├── vault/               # Local vault manager (filesystem-based)
│   │   ├── chain/               # [DEFERRED] Sui RPC client, proposals, audit, PTB
│   │   ├── seal/                # [DEFERRED] Seal encrypt/decrypt, access policies
│   │   ├── storage/             # SQLite (WAL mode) — task queue, token tracking
│   │   ├── utils/               # HTTP (libcurl), JSON (manual), crypto (ed25519), config
│   │   ├── sdk/                 # [DEFERRED] libquorum public API
│   │   └── cli/                 # quorum binary CLI commands
│   ├── tests/
│   └── configs/                 # Agent YAML definitions, task YAML definitions
│
├── scripts/                     # Shell scripts (smoke tests, utilities)
├── quorum-contracts/            # [DEFERRED] Move (OPEN SOURCE) — on-chain state machines
├── quorum-ts/                   # [DEFERRED] TypeScript (OPEN SOURCE) — community-facing
└── quorum-docs/                 # Documentation (OPEN SOURCE)
```

## Reference Codebase

The bot-manager project at `../bot-manager` is the primary C++ reference. Reuse its patterns for HTTP, JSON, crypto, SQLite, and general C++ style. When in doubt about how to implement something, check bot-manager first.

## Hard Constraints

### C++ Conventions (from bot-manager)

- **C++20 standard** — concepts, ranges, designated initializers, std::format
- **Namespace:** `sui::quorum`
- **No nlohmann/json** — use manual `json.h` parsing (copy pattern from bot-manager)
- **No Boost** — ever
- **No exceptions for control flow** — use return codes / std::optional / std::expected
- **Header-only preferred** for new modules (`.h` files with inline implementations)
- **Timestamps:** epoch seconds as `uint64_t` everywhere
- **Logging:** stdout/stderr only (match bot-manager; no spdlog unless explicitly decided)
- **String handling:** `std::string_view` for read-only, `std::string` for ownership
- **Error handling pattern:** return `bool` or `std::optional<T>` — never throw

### Dependencies (Phase 0)

| Dependency | Purpose | Source |
|-----------|---------|--------|
| sqlite3 | Task queue, token tracking, audit cache | Reuse from bot-manager |
| OpenSSL | Hashing (SHA-256), TLS | Reuse from bot-manager |
| libcurl | HTTP client (future use) | Reuse from bot-manager |

**Runtime dependency:** `claude` CLI (Claude Code) must be installed and authenticated.

**Banned:** nlohmann/json, Boost, any JSON library, any ORM, any framework

### Build System

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j$(nproc)

# Run daemon
./build/quorum_daemon --config configs/quorum.yaml

# Run tests
cd build && ctest --output-on-failure
```

### File Naming

- Headers: `snake_case.h` (e.g., `vault_manager.h`, `sui_client.h`)
- Sources: `snake_case.cpp` (only when not header-only)
- Tests: `test_<module>.cpp` (e.g., `test_scheduler.cpp`)
- Configs: `snake_case.yaml`
- Move: `snake_case.move`

## Architecture Rules

### The Daemon Is Deterministic

The orchestrator daemon (`quorum_daemon`) **never** calls an LLM. All scheduling, routing, consensus, and event handling is pure C++ logic. Agent invocations happen by spawning `claude -p` subprocesses (`src/agent/invoker.h`), triggered by the daemon's task dispatch loop.

### Agent Invocation (Phase 0)

The daemon spawns Claude Code CLI as the agent runtime:

```
daemon → assembles prompt (vault + task) → spawns `claude -p "..." --dangerously-skip-permissions --output-format json` → collects stdout → parses result → writes to vault → routes follow-up tasks
```

Each `claude -p` call is a **fresh context** (no memory between calls). The vault provides continuity.

### Parallelism Control

- Max concurrent `claude -p` processes: configurable (default 2-3)
- Per-task token cap: kills process if exceeded
- Global daily budget: daemon pauses all invocations when hit
- Critical for unattended overnight runs

### Proposal State Machine

```
DRAFT(0) → REVIEWING(1) → APPROVED(2) → EXECUTED(5) → EVALUATED(6)
                         → REJECTED(3)
                         → ESCALATED(4)
```

- Max 3 rounds per proposal
- Required reviewers declared at creation
- All transitions tracked locally in SQLite (Phase 0)
- On-chain transitions via PTB deferred to Phase 1+

### Vault System

Each agent owns a vault: `data/vaults/{agent_name}/`

```
data/vaults/{agent_name}/
├── CONTEXT.md          # Always loaded — role description + instructions
├── knowledge/          # Accumulated analysis and conclusions
├── experiments/        # Experiment designs and results
├── decisions/          # Past decisions linked to proposals
└── inbox/              # Items from other agents via proposal/consensus
```

- Local filesystem only (Phase 0) — no Walrus sync
- SQLite index for fast lookups
- Cross-agent reads through proposal review process (local enforcement)

### Storage (Phase 0)

```
Local only:  SQLite task queue, token tracking, vault index, proposal state
Future:      Walrus (vault blobs), Sui (proposals, audit), Seal (access control)
```

## Key Patterns

### HTTP Client Pattern (from bot-manager)

```cpp
// Use the libcurl wrapper from utils/http_client.h
// All HTTP calls go through this — Sui RPC, Walrus API, LLM APIs
auto response = sui::quorum::http::post(url, headers, body);
if (!response) {
    std::cerr << "HTTP error: " << response.error() << "\n";
    return std::nullopt;
}
```

### JSON Pattern (from bot-manager)

```cpp
// Manual JSON parsing — no libraries
// See utils/json.h for the parser
auto parsed = sui::quorum::json::parse(response_body);
auto value = parsed.get_string("result");
```

### SQLite Pattern (from bot-manager)

```cpp
// WAL mode, mutex-protected, RAII
sui::quorum::Database db("quorum.db");
db.execute("CREATE TABLE IF NOT EXISTS metrics (...)");
auto result = db.query("SELECT * FROM metrics WHERE agent = ?", agent_id);
```

### Config Pattern

```yaml
# configs/quorum.yaml — daemon config
daemon:
  pid_file: /tmp/quorum.pid
  data_dir: ./data
  log_level: info

chain:
  network: testnet          # testnet | mainnet
  rpc_url: https://fullnode.testnet.sui.io:443
  package_id: "0x..."

walrus:
  aggregator_url: https://aggregator.walrus-testnet.walrus.space
  publisher_url: https://publisher.walrus-testnet.walrus.space

agents:
  - config: configs/agents/market_analyst.yaml
  - config: configs/agents/bot_analyst.yaml
  - config: configs/agents/engineer.yaml
  - config: configs/agents/operator.yaml
```

## Move Contract Conventions (Deferred — Phase 1+)

- All proposal state transitions enforce the state machine (assert on invalid transitions)
- Use `clock::timestamp_ms(clock)` for all timestamps
- Capability pattern: `AgentCap` for agent auth, `HumanApprovalCap` for human gate
- Heavy content on Walrus (referenced by blob ID as `vector<u8>`), lightweight metadata on-chain
- Shared objects for proposals (multi-party interaction), owned objects for agent identities

## TypeScript Conventions (Deferred — Phase 5+)

- pnpm workspaces with turborepo
- @quorum/sdk connects to daemon via HTTP REST API
- @quorum/cli wraps SDK with commander.js
- @quorum/dashboard is React + Tailwind
- All open source

## What NOT To Do

1. **Never add LLM calls to the daemon control loop** — the orchestrator is deterministic
2. **Never use nlohmann/json or any JSON library** — manual json.h only
3. **Never use Boost** — for anything
4. **Never allow direct vault writes across agents** — cross-agent access goes through proposal review
5. **Never skip the proposal protocol** — all material decisions go through create→review→approve→execute→evaluate
6. **Never block the daemon event loop** — subprocess spawning must be non-blocking
7. **Never run `claude -p` without token budget enforcement** — per-task cap + global daily cap

## Current Phase

**Phase 0: Pure Local Orchestration** — Prove the multi-agent coordination loop on a single machine before adding web3 layers.

Priority order:
1. ~~C++ daemon skeleton~~ ✓ (main.cpp, signal handling, PID lock, config loading)
2. ~~Scheduler, message bus, router, event dispatcher~~ ✓ (skeleton implementations)
3. ~~Invoker rewrite~~ ✓ (spawns `claude -p`, captures JSON output, writes token/cost to DB)
4. ~~SQLite task queue~~ ✓ (pending/active/done states with token tracking)
5. ~~Context assembler~~ ✓ (vault CONTEXT.md + knowledge + inbox, output format instructions)
6. ~~Output parser~~ ✓ (VAULT_UPDATE / PROPOSAL / REVIEW / SUMMARY blocks, KV + multi-line parsing)
7. ~~Token budget enforcement~~ ✓ (per-task cap + hourly/daily caps with rolling window)
8. ~~Smoke test script~~ ✓ (scripts/smoke_test.sh — seeds tasks, runs daemon, validates results; includes WAL/SHM cleanup to prevent stale SQLite state)
9. ~~End-to-end dispatch verified~~ ✓ (daemon claims pending tasks, invokes `claude -p`, writes results back to DB)
10. **`quorum status` CLI** — check overnight run results (tasks completed, tokens spent, errors)

**Goal:** Daemon spawns `claude -p` processes, manages task queue, coordinates multiple agents through filesystem vaults. Fully automated, runs unattended for hours.

### Known Issues

- **Nested Claude Code sessions:** `claude -p` refuses to launch inside another Claude Code session (`CLAUDECODE` env var detected). The smoke test must be run from a regular terminal, not from within `claude` CLI.
- **Invoker error handling:** The invoker marks tasks `done` even when `claude -p` exits non-zero (as long as stdout is non-empty). Error messages get stored as "results" instead of triggering `failed` status. Needs exit-code + JSON validation before calling `mark_done`.
- **Buffered stdout in background mode:** When daemon stdout is redirected to a file, `std::cout` uses full buffering. Verbose log lines only appear after process exit. Add `std::flush` to verbose output paths if real-time log tailing is needed.

## Useful Commands

```bash
# Build
cmake -B build && cmake --build build -j$(nproc)

# Run daemon
./build/quorum_daemon --config configs/quorum.yaml

# Run with verbose logging
./build/quorum_daemon --config configs/quorum.yaml --verbose

# Check daemon status (overnight runs)
./build/quorum status

# CLI commands (Phase 0)
./build/quorum daemon start
./build/quorum daemon status
./build/quorum agent list
./build/quorum proposal create --title "..." --author market_analyst
./build/quorum proposal status --id proposal-042

# Agent invocation (what the daemon spawns)
claude -p "prompt" --dangerously-skip-permissions --output-format json

# Smoke test (seeds tasks, runs daemon with real claude -p, validates results)
# MUST be run from a regular terminal, NOT inside a claude session
./scripts/smoke_test.sh

# Troubleshooting: clear stale SQLite WAL/SHM if daemon sees wrong data
rm -f data/quorum.db-wal data/quorum.db-shm
```
