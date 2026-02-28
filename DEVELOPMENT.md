# Quorum — Development Guide

## Current Phase

**Phase 1: Orchestrator Daemon + Sui Contracts** (Weeks 1-4)

## Implementation Order

### Week 1: Daemon Skeleton

1. **`src/main.cpp`** — entry point
   - Signal handling (SIGINT, SIGTERM, SIGUSR1)
   - PID file lock (`/tmp/quorum.pid`)
   - Config loading (YAML or manual JSON)
   - Main event loop (start scheduler, message bus)

2. **`src/utils/config.h`** — config loader
   - Parse `quorum.yaml` (or JSON equivalent)
   - Validate required fields
   - Return strongly-typed config struct

3. **`src/utils/json.h`** — manual JSON parser
   - Copy/adapt from bot-manager's json.h
   - parse(), get_string(), get_int(), get_array(), get_object()

4. **`src/utils/http_client.h`** — libcurl wrapper
   - Copy/adapt from bot-manager
   - GET, POST with headers and body
   - Timeout handling

5. **`src/storage/database.h`** — SQLite wrapper
   - Copy/adapt from bot-manager
   - WAL mode, mutex, RAII
   - execute(), query(), prepare()

### Week 2: Daemon Components

6. **`src/daemon/message_bus.h`** — in-process message queue
   - Thread-safe queue (mutex + condition variable)
   - Message types: INVOKE_AGENT, PROPOSAL_EVENT, SCHEDULE_TRIGGER, SHUTDOWN
   - publish(), subscribe(), poll()

7. **`src/daemon/scheduler.h`** — task scheduler
   - Periodic tasks (cron-like, configurable interval)
   - Timer tasks (one-shot delayed)
   - Event tasks (triggered by message bus)
   - Uses agent YAML configs for schedule definitions

8. **`src/daemon/router.h`** — task routing
   - Static rules: task type → agent mapping
   - Priority handling (consensus review > scheduled tasks)
   - Prevent concurrent invocation of same agent

9. **`src/daemon/consensus.h`** — proposal state machine
   - Local mirror of on-chain proposal states
   - State transitions: DRAFT→REVIEWING→APPROVED/REJECTED/ESCALATED→EXECUTED→EVALUATED
   - Round tracking, timeout detection
   - Sync with on-chain state (when chain module ready)

10. **`src/daemon/event_dispatcher.h`** — external event detection
    - File system watching (new data files)
    - SQLite change hooks (metric thresholds)
    - Manual triggers (SIGUSR1/SIGUSR2)

### Week 3: Agent Invocation

11. **`src/agent/invoker.h`** — LLM API caller
    - Anthropic API (Claude) — POST to /v1/messages
    - Build messages array with system prompt + context
    - Parse response, extract structured output blocks
    - Timeout and retry logic

12. **`src/agent/context_assembler.h`** — prompt builder
    - Read agent CONTEXT.md from vault
    - Select relevant vault files (knowledge, experiments, decisions)
    - Query SQLite for recent metrics
    - Assemble into single prompt within token budget
    - Token counting (approximate: chars / 4)

13. **`src/agent/output_parser.h`** — structured output parser
    - Parse VAULT_UPDATE blocks → file writes
    - Parse PROPOSAL blocks → proposal creation
    - Parse REVIEW blocks → review submission
    - Parse SUMMARY blocks → logging
    - Handle malformed output gracefully

14. **`src/agent/model_router.h`** — tier selection
    - Task type → inference tier mapping (from agent YAML)
    - Local LLM endpoint (Ollama) for Tier 1
    - Frontier API (Claude) for Tier 2
    - Fallback: if local fails, escalate to frontier

### Week 4: Sui Integration + CLI

15. **`src/chain/sui_client.h`** — Sui RPC client
    - Copy/adapt from bot-manager
    - sui_getObject, sui_executeTransactionBlock
    - Transaction building and signing

16. **`src/chain/proposal.h`** — proposal management
    - Create proposal on-chain
    - Submit review on-chain
    - Query proposal status
    - Record execution and outcome

17. **`src/chain/agent_identity.h`** — agent registry
    - Register agent on-chain
    - Query agent identity
    - Update last_active timestamp

18. **`src/chain/audit.h`** — audit logging
    - Log events on-chain (lightweight pointers)
    - Query audit history

19. **`src/cli/cli_main.cpp`** — CLI entry point
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
cmake -B build -S quorum-core && cmake --build build -j$(nproc)

# Run daemon
./build/quorum_daemon --config configs/quorum.yaml

# Build Move contracts
cd quorum-contracts && sui move build && sui move test

# Deploy to testnet
cd quorum-contracts && sui client publish --gas-budget 100000000
```

## Phase 1 Success Criteria

- [ ] Daemon starts, loads config, creates PID file
- [ ] Scheduler triggers periodic agent tasks
- [ ] Agent invoker successfully calls Claude API
- [ ] Context assembler builds prompts from vault files
- [ ] Output parser extracts structured blocks
- [ ] Proposal state machine tracks lifecycle locally
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
