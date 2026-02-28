# Quorum

**Verifiable Multi-Agent Orchestration on Sui Stack**

> Your AI agents now have verifiable memory and auditable decisions.

Quorum is a domain-specialized multi-agent orchestration framework where independent AI agents coordinate through structured proposals, persist knowledge on [Walrus](https://walrus.site), record decisions on [Sui](https://sui.io), and enforce access control via [Seal](https://docs.seal.mystenlabs.com).

## What Makes Quorum Different

Every existing multi-agent framework (CrewAI, LangGraph, AG2) operates on implicit trust — you trust that agent memory is accurate, that decisions were fair, that audit logs haven't been tampered with.

Quorum replaces trust with cryptographic verification:

- **Agent knowledge** → Walrus (content-addressed, versioned, encrypted)
- **Decisions** → Sui on-chain (tamper-proof, atomic via PTB)
- **Access control** → Seal (threshold encryption, Move-defined policies)
- **Orchestration** → Deterministic C++ daemon (zero LLM in the control loop)

## Architecture

```
Orchestrator Daemon (C++20, deterministic, no LLM)
    │
    ├── Scheduler (cron / timer / event)
    ├── Router (static rules)
    ├── Consensus Engine (proposal state machine)
    └── Event Dispatcher (file watch / DB triggers / chain events)
         │
    ┌────┴────┐
    │ Agents  │  ← LLM calls happen here (Tier 1: local, Tier 2: frontier)
    └────┬────┘
         │
    Local Layer ─── SQLite metrics, tmp workspace
    Walrus Layer ── Vault blobs, snapshots, Seal-encrypted data
    Sui Layer ───── Proposal objects, agent identities, audit log
```

## Quick Start

```bash
# Bootstrap project structure
make init

# Build C++ daemon
make build

# Run on Sui testnet
./build/quorum_daemon --config configs/quorum.yaml

# Deploy Move contracts
make deploy-contracts
```

## Project Structure

| Directory | Language | Visibility | Purpose |
|-----------|----------|-----------|---------|
| `quorum-core/` | C++20 | Closed source | Orchestrator daemon, SDK, CLI |
| `quorum-contracts/` | Move | Open source | On-chain proposal, agent, audit contracts |
| `quorum-ts/` | TypeScript | Open source | SDK, CLI wrapper, dashboard |
| `quorum-docs/` | Markdown | Open source | Documentation |

## Sui Stack Integration

| Component | Sui Stack | Purpose |
|-----------|-----------|---------|
| Agent Vault | Walrus | Content-addressed, versioned, deletable blob storage |
| Proposals | Sui Objects + Move | On-chain state machine with atomic transitions (PTB) |
| Access Control | Seal | Threshold encryption with Move-defined policies |
| Audit Log | Sui Transactions | Tamper-proof decision records |
| Agent Identity | Sui Owned Objects | Capability-based auth, portable, composable |

## Proposal Protocol

All material decisions follow the proposal lifecycle:

```
DRAFT → REVIEWING → APPROVED → EXECUTED → EVALUATED
                  → REJECTED
                  → ESCALATED (human decides after 3 rounds)
```

## License

C++ core: Proprietary
Move contracts: Apache 2.0
TypeScript packages: Apache 2.0
Documentation: CC BY 4.0
