# Quorum — Development Guide

## Current Phase

**Phase 1: Orchestrator Daemon + Sui Contracts** (Weeks 1-4)

## Implementation Order

### Week 1: Daemon Skeleton ✅

1. **`src/main.cpp`** ✅ — entry point
   - Signal handling (SIGINT, SIGTERM)
   - PID file lock (`/tmp/quorum.pid`)
   - Config loading (manual YAML parser)
   - Main event loop (scheduler, message bus, event dispatcher)
   - SQLite schema creation (audit_log, proposals tables)

2. **`src/utils/config.h`** ✅ — config loader
   - Parses `quorum.yaml` with built-in minimal YAML parser
   - Strongly-typed `QuorumConfig` struct with all sections
   - No yaml-cpp dependency required at runtime

3. **`src/utils/json.h`** ✅ — manual JSON parser
   - `extract_string()`, `extract_number()`, `extract_int()`, `extract_bool()`
   - `extract_anthropic_content()` for Anthropic Messages API response parsing
   - `build_object()`, `quote()` for JSON construction
   - No external JSON library

4. **`src/utils/http_client.h`** ✅ — libcurl wrapper
   - GET, POST with JSON body and custom headers
   - `post_json()` overload accepting caller-supplied extra headers
   - Configurable timeout and user agent
   - Non-copyable, non-movable RAII design

5. **`src/storage/database.h`** ✅ — SQLite wrapper
   - WAL mode, mutex-protected, RAII
   - `execute()`, `query()` with lambda bind/callback
   - `Transaction` guard with auto-rollback

### Week 2: Daemon Components ✅

6. **`src/daemon/message_bus.h`** ✅ — in-process message queue
   - Thread-safe queue (mutex-protected)
   - Topic-based `publish()` / `subscribe()` / `drain()`
   - Message struct with topic, sender, payload, timestamp

7. **`src/daemon/scheduler.h`** ✅ — task scheduler
   - Periodic tasks with configurable interval
   - `tick(now)` driven by main loop
   - Tracks last run time per task

8. **`src/daemon/router.h`** ✅ — task routing
   - Static rules: task type → agent name mapping
   - `add_route()` / `resolve()` interface

9. **`src/daemon/consensus.h`** ✅ — proposal state machine
   - `LocalProposal` struct with full lifecycle state
   - `ProposalState` enum matching Move contract (0-6)
   - `can_transition()` / `transition()` with validation
   - MAX_ROUNDS = 3

10. **`src/daemon/event_dispatcher.h`** ✅ — event system
    - `on()` / `emit()` pattern for internal events
    - Used by daemon main loop for lifecycle hooks

### Week 3: Agent Invocation (scaffolded)

11. **`src/agent/invoker.h`** ✅ — `claude -p` subprocess runner
    - `invoke(task_id)` reads prompt from DB, writes to temp file, spawns `claude -p`
    - Command: `cat prompt.txt | claude -p --dangerously-skip-permissions --output-format json`
    - Parses JSON envelope from stdout: extracts `result`, `cost_usd`, `input_tokens`, `output_tokens`
    - Marks task `done` or `failed` in SQLite with token/cost data
    - Needs: retry logic, per-task token cap enforcement

12. **`src/agent/context_assembler.h`** ✅ — prompt builder
    - Loads `CONTEXT.md` (always first), then `knowledge/` files by recency, then `inbox/` items
    - Respects `ContextBudget` (max 20 files, 200 000 chars)
    - Appends task description and structured output format instructions
    - Output instructions teach agents the 4 block types: `VAULT_UPDATE`, `PROPOSAL`, `REVIEW`, `SUMMARY`

13. **`src/agent/output_parser.h`** ✅ — structured output parser
    - Scans agent response line-by-line; detects fenced block opens (`` ```BLOCK_TYPE ``) and closes (`` ``` ``)
    - Parses 4 block types: `VAULT_UPDATE` → `VaultUpdate`, `PROPOSAL` → `Proposal`, `REVIEW` → `Review`, `SUMMARY` → `summary` string
    - Key-value parser inside blocks handles: simple `key: value`, `key: [list, items]`, and `key: |` multi-line with 2-space indent stripping
    - Everything outside blocks collected into `free_text`; original preserved in `raw`
    - `has_actionable_output()` returns true when any vault update, proposal, or review was parsed
    - Lenient: unclosed blocks are parsed rather than dropped

14. **`src/agent/model_router.h`** ✅ — tier selection
    - `InferenceTier` enum (RuleBased, LocalLLM, Frontier, Human)
    - `route()` maps task type to tier deterministically

### Week 4: Sui Integration + CLI (scaffolded)

15. **`src/chain/sui_client.h`** — Sui RPC client (scaffolded)
    - `get_object()` via JSON-RPC
    - Needs: `executeTransactionBlock`, transaction signing

16. **`src/chain/proposal.h`** — proposal management (stub)
    - Pending Move contract deployment

17. **`src/chain/agent_identity.h`** — agent registry (stub)
    - Pending Move contract deployment

18. **`src/chain/audit.h`** — audit logging (stub)
    - Pending Move contract deployment

19. **`src/cli/cli_main.cpp`** — CLI entry point (stub)
    - Not yet wired into build
    - `quorum daemon start|stop|status`
    - `quorum agent list|create|invoke`
    - `quorum proposal create|status|list`
    - `quorum vault list|read`
    - `quorum audit list`

20. **Move contracts** — deploy to testnet
    - `proposal.move` — full state machine
    - `agent.move` — identity + capability
    - `audit.move` — append-only log
    - `vault_access.move` — Seal policies (stub for Phase 3)

## Build & Test

```bash
# Build
cd quorum-core && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(sysctl -n hw.ncpu)

# Run daemon
./build/quorum_daemon --config ../configs/quorum.yaml

# Run with verbose logging
./build/quorum_daemon --config ../configs/quorum.yaml --verbose

# Build Move contracts
cd quorum-contracts && sui move build && sui move test

# Deploy to testnet
cd quorum-contracts && sui client publish --gas-budget 100000000
```

### Dependencies (macOS)

```bash
brew install openssl@3 sqlite yaml-cpp
# curl and sqlite3 are provided by Xcode SDK
```

## Phase 1 Success Criteria

- [x] Daemon starts, loads config, creates PID file
- [x] Scheduler triggers periodic agent tasks
- [x] Agent invoker spawns `claude -p` subprocesses, tracks token usage
- [x] Context assembler builds prompts from vault files
- [x] Output parser extracts VAULT_UPDATE / PROPOSAL / REVIEW / SUMMARY blocks
- [x] Proposal state machine tracks lifecycle locally
- [ ] Move contracts deployed to Sui testnet
- [ ] CLI can create proposals and query status
- [ ] 5 complete proposal cycles end-to-end
- [ ] On-chain verification of proposal states

## Subsequent Phases

| Phase | Focus | Weeks |
|-------|-------|-------|
| 2 | Walrus vault integration | 5-6 |
| 3 | Seal access control | 7-8 |
| 4 | Local LLM + cost optimization | 9-10 |
| 5 | TypeScript SDK + dashboard | 11-12 |
| 6 | Outcome learning | 13-14 |
| 7 | Mainnet + grant | 15-16 |
