# Quorum Architecture

## Overview

Quorum is a **verifiable multi-agent orchestration framework** built on the Sui Stack. It coordinates independent AI agents that accumulate domain knowledge, make decisions through structured proposals, and record everything with cryptographic guarantees.

## Design Principles

1. **Deterministic orchestration, non-deterministic reasoning.** The C++ daemon never calls an LLM. LLMs do analysis and generation; compiled code does routing, scheduling, and consensus.

2. **Context separation is the product.** Each agent gets its own full context window loaded with domain-relevant knowledge. A Market Analyst and an Engineer need fundamentally different information.

3. **Persistent memory as first-class citizen.** Agent knowledge accumulates across sessions in Walrus-backed vaults — version-controlled, queryable, and surviving indefinitely.

4. **Cost-layered inference.** Tier 0: rules (free). Tier 1: local LLM ($0). Tier 2: frontier model ($$). Tier 3: human. Target: 90% local, 10% frontier.

5. **Fail-safe by default.** If the orchestrator crashes, underlying systems continue with their last known configuration.

6. **Local speed, on-chain truth.** The orchestrator runs locally for speed. Chain interactions happen at boundaries only (proposal transitions, audit entries, vault persistence).

## System Layers

### Orchestrator Daemon (C++20)

A single long-running process that manages all agent invocations deterministically. No LLM calls in this layer.

**Components:**
- **Scheduler** — Periodic (cron), timer (one-shot), and event-driven triggers
- **Router** — Maps tasks to agents via static rules with priority handling
- **Consensus Engine** — Tracks proposal state machine, enforces round limits
- **Event Dispatcher** — Monitors file system, database, and chain for changes
- **Message Bus** — In-process thread-safe queue connecting all components

### Agent Layer

Each agent is invoked as an independent LLM call. The orchestrator assembles context (vault files + metrics + task description), calls the LLM, and parses structured output.

Agents are **stateless between invocations** — all state lives in their vault and the shared data layer.

### Storage Layers

**Local (Tier 0):** SQLite for metrics, temporary workspace, local LLM inference. Free.

**Walrus (Tier 1):** Agent vault blobs, periodic snapshots, Seal-encrypted cross-agent data. Low cost, content-addressed, deletable.

**Sui (Tier 2):** Proposal objects, agent identities, audit log entries. Decisions only, tamper-proof.

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
- Human approval gate for high-stakes decisions (via HumanApprovalCap)
- All transitions are atomic on Sui via Programmable Transaction Blocks

## Agent Roles (Default Template)

The four-role pattern maps to any domain:

| Role | Core Question | Looks At |
|------|--------------|----------|
| **External Analyst** | "Where can we make money?" | Markets, competitors, opportunities |
| **Internal Analyst** | "Is our system performing?" | Metrics, P&L, experiments |
| **Engineer** | "How do we build it safely?" | Code, architecture, safety |
| **Operator** | "Is everything running?" | Processes, logs, configs |

## Sui Stack Mapping

| Quorum Concept | Sui Stack | Why It Fits |
|----------------|-----------|-------------|
| Agent Vault | Walrus | Content-addressed, versioned, deletable, programmable metadata |
| Proposal Protocol | Sui Objects + Move | State machine as object lifecycle, atomic via PTB |
| Access Control | Seal | Threshold encryption with Move-defined policies |
| Audit Log | Sui Transactions | Tamper-proof by construction |
| Agent Identity | Sui Owned Objects | Capability-based auth, portable |
