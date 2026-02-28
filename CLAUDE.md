# Quorum — Claude Code Instructions

## Project Overview

Quorum is a **verifiable multi-agent orchestration framework** on Sui Stack. A deterministic C++20 daemon orchestrates independent AI agents that coordinate through on-chain proposals, persist knowledge on Walrus, and enforce access control via Seal.

**Tagline:** "Your AI agents now have verifiable memory and auditable decisions."

## Repo Layout

```
quorum/
├── CLAUDE.md                    ← You are here
├── quorum-core/                 # C++20 (CLOSED SOURCE) — orchestrator daemon
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── main.cpp             # Daemon entry, signal handling, PID lock
│   │   ├── daemon/              # Scheduler, router, consensus, events, message bus
│   │   ├── agent/               # LLM invoker, context assembler, output parser, model router
│   │   ├── vault/               # Walrus vault manager, retention, indexer
│   │   ├── chain/               # Sui RPC client, proposal mgmt, agent identity, audit, PTB
│   │   ├── seal/                # Seal encrypt/decrypt, access policies
│   │   ├── storage/             # SQLite (WAL mode), local vault cache
│   │   ├── utils/               # HTTP (libcurl), JSON (manual), crypto (ed25519), config
│   │   ├── sdk/                 # libquorum public API
│   │   └── cli/                 # quorum binary CLI commands
│   ├── tests/
│   └── configs/                 # Agent YAML definitions, task YAML definitions
│
├── quorum-contracts/            # Move (OPEN SOURCE) — on-chain state machines
│   ├── Move.toml
│   ├── sources/
│   │   ├── proposal.move        # DRAFT→REVIEWING→APPROVED→EXECUTED→EVALUATED
│   │   ├── agent.move           # AgentIdentity + AgentCap
│   │   ├── audit.move           # Append-only audit log
│   │   └── vault_access.move    # Seal access policies
│   └── tests/
│
├── quorum-ts/                   # TypeScript (OPEN SOURCE) — community-facing
│   ├── packages/sdk/            # @quorum/sdk
│   ├── packages/cli/            # @quorum/cli (npm wrapper)
│   └── packages/dashboard/      # @quorum/dashboard (React web UI)
│
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

### Dependencies

| Dependency | Purpose | Source |
|-----------|---------|--------|
| libcurl | HTTP client (Sui RPC, Walrus API, LLM APIs) | Reuse from bot-manager |
| sqlite3 | Local metrics, audit cache, vault index | Reuse from bot-manager |
| ed25519 | Sui transaction signing | Reuse from bot-manager |
| OpenSSL | Hashing (SHA-256, BLAKE2), TLS | Reuse from bot-manager |
| yaml-cpp | Config parsing (agent/task YAML) | New dependency |
| libuv | Event loop, file watching, timers | New dependency (evaluate) |
| CLI11 | CLI argument parsing | New dependency |

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

The orchestrator daemon (`quorum_daemon`) **never** calls an LLM. All scheduling, routing, consensus, and event handling is pure C++ logic. LLM calls happen only in the agent invocation layer (`src/agent/invoker.h`), which is triggered by the daemon but runs as an external API call.

### Cost-Layered Inference

```
Tier 0: Rule-based (free)      — ALL daemon logic
Tier 1: Local LLM ($0)         — 90% of LLM calls (Ollama/llama.cpp)
Tier 2: Frontier model ($$)    — 10% of LLM calls (Claude API)
Tier 3: Human (priceless)      — capital decisions, deadlocks
```

The `model_router.h` selects tier based on task type, never on LLM output.

### Proposal State Machine

```
DRAFT(0) → REVIEWING(1) → APPROVED(2) → EXECUTED(5) → EVALUATED(6)
                         → REJECTED(3)
                         → ESCALATED(4)
```

- Max 3 rounds per proposal
- Required reviewers declared at creation
- Human approval gate for high-stakes decisions (HumanApprovalCap)
- All transitions are atomic on-chain via PTB

### Vault System

Each agent owns a vault: `vault/{agent_name}/`

```
vault/{agent_name}/
├── CONTEXT.md          # Always loaded — role description + instructions
├── knowledge/          # Accumulated analysis and conclusions
├── experiments/        # Experiment designs and results
├── decisions/          # Past decisions linked to on-chain proposals
└── inbox/              # Items from other agents via proposal/consensus
```

- Local SQLite cache for fast reads
- Async sync to Walrus (content-addressed blobs)
- Cross-agent reads ONLY through Seal-authorized proposal review
- Retention policies: permanent | archive_after_90d | archive_after_evaluation

### Three-Layer Storage

```
Local (Tier 0):   SQLite metrics, tmp workspace, local LLM — FREE
Walrus (Tier 1):  Vault blobs, snapshots, encrypted cross-agent data — LOW COST
Sui (Tier 2):     Proposal objects, agent identities, audit log — DECISIONS ONLY
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

## Move Contract Conventions

- All proposal state transitions enforce the state machine (assert on invalid transitions)
- Use `clock::timestamp_ms(clock)` for all timestamps
- Capability pattern: `AgentCap` for agent auth, `HumanApprovalCap` for human gate
- Heavy content on Walrus (referenced by blob ID as `vector<u8>`), lightweight metadata on-chain
- Shared objects for proposals (multi-party interaction), owned objects for agent identities

## TypeScript Conventions

- pnpm workspaces with turborepo
- @quorum/sdk connects to daemon via HTTP REST API
- @quorum/cli wraps SDK with commander.js
- @quorum/dashboard is React + Tailwind
- All open source

## What NOT To Do

1. **Never add LLM calls to the daemon control loop** — the orchestrator is deterministic
2. **Never use nlohmann/json or any JSON library** — manual json.h only
3. **Never use Boost** — for anything
4. **Never store full proposal content on Sui** — only metadata + Walrus blob IDs
5. **Never allow direct vault writes across agents** — cross-agent access goes through Seal
6. **Never skip the proposal protocol** — all material decisions go through create→review→approve→execute→evaluate
7. **Never hardcode Sui addresses** — all object IDs come from config
8. **Never block the daemon event loop** — async I/O for chain and Walrus calls

## Current Phase

**Phase 1: Orchestrator Daemon + Sui Contracts** (Weeks 1-4)

Priority order:
1. C++ daemon skeleton: main.cpp, signal handling, PID lock, config loading
2. Scheduler (cron + timer + event)
3. Message bus (in-process queue)
4. Router (static rules)
5. Consensus engine (proposal state machine — local mirror)
6. Agent invoker (Claude API wrapper)
7. Move contracts: proposal.move, agent.move, audit.move
8. CLI: `quorum daemon start/stop/status`, `quorum proposal create/status`

## Useful Commands

```bash
# Build
cmake -B build && cmake --build build -j$(nproc)

# Run daemon
./build/quorum_daemon --config configs/quorum.yaml

# Run with verbose logging
./build/quorum_daemon --config configs/quorum.yaml --verbose

# CLI commands
./build/quorum daemon start
./build/quorum daemon status
./build/quorum agent list
./build/quorum proposal create --title "..." --author market_analyst
./build/quorum proposal status --id proposal-042
./build/quorum vault list --agent market_analyst
./build/quorum audit list --limit 20

# Move contract build (from quorum-contracts/)
sui move build
sui move test

# Move deploy to testnet
sui client publish --gas-budget 100000000

# TypeScript (from quorum-ts/)
pnpm install
pnpm build
pnpm --filter @quorum/cli start
```
