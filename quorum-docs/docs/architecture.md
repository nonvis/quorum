# Quorum Architecture

## Overview

Quorum is a **multi-agent orchestration framework**. A deterministic C++20 daemon orchestrates independent AI agents that accumulate domain knowledge, make decisions through structured proposals, and coordinate via local vaults.

**Phase 0 (current):** Pure local orchestration. No blockchain, no remote storage. The daemon spawns `claude -p` (Claude Code CLI) subprocesses as the agent runtime. Web3 layers (Sui, Walrus, Seal) will be added in later phases.

## Design Principles

1. **Deterministic orchestration, non-deterministic reasoning.** The C++ daemon never calls an LLM. LLMs do analysis and generation; compiled code does routing, scheduling, and consensus.

2. **Context separation is the product.** Each agent gets its own full context window loaded with domain-relevant knowledge. A Market Analyst and an Engineer need fundamentally different information.

3. **Persistent memory as first-class citizen.** Agent knowledge accumulates across sessions in local vaults — queryable and surviving indefinitely. (Walrus-backed persistence in later phases.)

4. **Fail-safe by default.** If the orchestrator crashes, underlying systems continue with their last known configuration.

5. **Local speed, verifiable truth (later).** The orchestrator runs locally for speed. Chain interactions will happen at boundaries only in future phases.

## System Layers

### Orchestrator Daemon (C++20)

A single long-running process that manages all agent invocations deterministically. No LLM calls in this layer.

**Components (all implemented as skeletons):**
- **Scheduler** (`daemon/scheduler.h`) — Periodic interval-driven triggers with per-task tracking
- **Router** (`daemon/router.h`) — Maps task types to agents via static rules
- **Consensus Engine** (`daemon/consensus.h`) — Proposal state machine (DRAFT→EVALUATED) with round limits
- **Event Dispatcher** (`daemon/event_dispatcher.h`) — Event pub/sub for internal lifecycle hooks
- **Message Bus** (`daemon/message_bus.h`) — Thread-safe topic-based queue connecting all components

### Agent Layer (Phase 0)

Each agent is invoked by spawning a `claude -p` subprocess. The daemon:

1. Assembles context (vault CONTEXT.md + relevant vault files + task description)
2. Spawns: `claude -p "prompt" --dangerously-skip-permissions --output-format json`
3. Collects stdout when process completes
4. Parses structured output (vault updates, proposals, reviews)
5. Writes results to agent's vault
6. Routes follow-up tasks to other agents

Agents are **stateless between invocations** — all state lives in their vault. Each `claude -p` call is a fresh context. The vault provides continuity.

**Parallelism:** Max 2-3 concurrent `claude -p` processes (configurable). Per-task token cap + global daily budget prevent runaway costs during unattended runs.

### Storage (Phase 0)

**Local only:**
- SQLite (WAL mode) — task queue, token tracking, proposal state, vault index
- Filesystem — agent vault directories under `data/vaults/`

**Deferred:**
- Walrus — vault blob persistence (Phase 1+)
- Sui — proposal objects, agent identities, audit log (Phase 1+)
- Seal — cross-agent access control (Phase 2+)

## Proposal Protocol

The primary coordination mechanism between agents:

```
DRAFT(0) → REVIEWING(1) → APPROVED(2) → EXECUTED(5) → EVALUATED(6)
                         → REJECTED(3)
                         → ESCALATED(4) → Human decides
```

**Rules:**
- Max 3 rounds of review per proposal
- Required reviewers declared at creation time
- Human approval gate for high-stakes decisions
- All transitions tracked in SQLite (Phase 0), on-chain via PTB (Phase 1+)

## Agent Roles (Default Template)

The four-role pattern maps to any domain:

| Role | Core Question | Looks At |
|------|--------------|----------|
| **External Analyst** | "Where can we make money?" | Markets, competitors, opportunities |
| **Internal Analyst** | "Is our system performing?" | Metrics, P&L, experiments |
| **Engineer** | "How do we build it safely?" | Code, architecture, safety |
| **Operator** | "Is everything running?" | Processes, logs, configs |

## Future: Sui Stack Mapping (Phase 1+)

| Quorum Concept | Sui Stack | Why It Fits |
|----------------|-----------|-------------|
| Agent Vault | Walrus | Content-addressed, versioned, deletable, programmable metadata |
| Proposal Protocol | Sui Objects + Move | State machine as object lifecycle, atomic via PTB |
| Access Control | Seal | Threshold encryption with Move-defined policies |
| Audit Log | Sui Transactions | Tamper-proof by construction |
| Agent Identity | Sui Owned Objects | Capability-based auth, portable |
