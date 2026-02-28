# Quorum — Core Concepts

A glossary of everything you need to understand to use Quorum effectively.

---

## Agent

An agent is a **role, not a process**. It doesn't run continuously — it gets invoked by the daemon when its schedule triggers or an event fires. Each invocation is a single LLM call with assembled context.

An agent is defined by three things:
- **YAML config** (`agents/*.yaml`) — schedule, triggers, inference tier, context budget
- **Vault CONTEXT.md** — who it is, what it knows, what it does and doesn't do
- **Vault knowledge files** — accumulated findings, experiments, decisions

Between invocations, an agent has no state. Everything it "remembers" comes from its vault, which the daemon loads into the LLM prompt.

**Key insight:** Agents are cheap to define and expensive to run well. A bad CONTEXT.md produces bad outputs regardless of model quality. Writing good agent instructions is the primary skill.

---

## Vault

A vault is an **agent's memory on disk** — a folder of markdown files that grows over time.

```
vaults/bot_analyst/
├── CONTEXT.md          # Always loaded (role identity)
├── knowledge/          # Conclusions and ongoing analysis
├── experiments/        # Experiment designs and results
├── decisions/          # Past proposal outcomes
└── inbox/              # Items from other agents
```

The daemon's **context assembler** selects which vault files to include in each invocation, staying within the agent's token budget. CONTEXT.md always loads. Other files are selected by recency, relevance, and the `always_include` list.

Vaults sync to **Walrus** for persistence and verifiability. Locally, they're plain markdown files you can read, edit, or seed manually.

**What makes vaults different from other agent memory:**
- They're files, not embeddings — you can read them, version them, diff them
- Agents write their own memory (structured output → vault updates)
- Cross-agent reads require Seal authorization (no silent access)
- Everything is content-addressed on Walrus (tamper-evident)

---

## Proposal

A proposal is a **structured request for a decision** that goes through multi-agent review before execution.

### Lifecycle

```
DRAFT → REVIEWING → APPROVED → EXECUTED → EVALUATED
                  → REJECTED
                  → ESCALATED (human decides)
```

### Anatomy

Every proposal declares:
- **Author** — which agent created it
- **Required reviewers** — which agents must approve (e.g., `[bot_analyst, engineer]`)
- **Informed** — agents who see it but don't need to sign off
- **Human gate** — whether human approval is needed after agent consensus
- **Content** — the actual analysis and recommendation (stored on Walrus)

### Why Proposals Exist

Proposals prevent two failure modes:
1. **Unilateral action** — a single agent making changes nobody validated
2. **Implicit decisions** — changes happening without any record of why

Even if you run a single agent, proposals create an auditable decision trail.

### The 3-Round Rule

A proposal gets at most 3 rounds of review. If reviewers still disagree after round 3, the proposal escalates to the human. This prevents infinite deliberation loops.

---

## Consensus

Consensus is **agreement between required reviewers** on a proposal. It's tracked by the daemon's consensus engine (locally) and mirrored on Sui (on-chain).

### How It Works

1. Author submits proposal → status becomes REVIEWING
2. Each required reviewer submits a verdict: **APPROVE**, **REVISE**, or **REJECT**
3. If all approve → APPROVED
4. If anyone says REVISE → author revises, next round begins
5. If anyone says REJECT → REJECTED (with justification)
6. If max rounds reached without agreement → ESCALATED to human

### On-Chain Recording

Each verdict is a Sui transaction. The proposal is a shared Sui object whose state transitions are atomic (via PTB). This means:
- Nobody can fake an approval after the fact
- The timeline of reviews is cryptographically verifiable
- The human approval is their wallet signature on a Sui transaction

---

## Daemon

The daemon is a **single long-running C++ process** that orchestrates everything. It has five components:

| Component | What It Does |
|-----------|-------------|
| **Scheduler** | Fires agent tasks on cron, timer, or event triggers |
| **Router** | Maps tasks to agents, handles priority, prevents double-invocation |
| **Consensus Engine** | Tracks proposal state machine, enforces round limits |
| **Event Dispatcher** | Watches files, SQLite, and chain for changes |
| **Message Bus** | In-process queue connecting all components |

**Critical property:** The daemon is 100% deterministic. It never calls an LLM. All scheduling, routing, and consensus logic is pure compiled code. LLM calls happen only in the agent invocation layer, which the daemon triggers but does not participate in.

This means:
- If the LLM is down, the daemon keeps running (just can't invoke agents)
- If the chain is down, the daemon keeps running (queues transactions)
- Daemon behavior is reproducible and debuggable — no "the AI decided to skip this step"

---

## Inference Tiers

Quorum uses a **cost-layered model** for all computation:

| Tier | What | Cost | Used For |
|------|------|------|----------|
| **0** | Daemon rules | $0 | Scheduling, routing, consensus, event handling |
| **1** | Local LLM | $0 (GPU power) | Routine scans, classification, status reports |
| **2** | Frontier model | ~$0.05/call | Deep analysis, proposal authoring, code review |
| **3** | Human | Priceless | Capital decisions, deadlocks, emergencies |

The **model router** selects the tier based on task type (defined in agent YAML), never on LLM output. This keeps costs predictable.

**Target ratio:** 90% Tier 1 (local), 10% Tier 2 (frontier). A typical four-agent system costs ~$10-15/month in LLM API fees.

---

## Walrus

[Walrus](https://walrus.site) is **decentralized blob storage** built on Sui. Quorum uses it for vault persistence.

What Walrus provides that local files don't:
- **Content addressing** — same content = same blob ID (tamper-evident)
- **Versioning** — old versions persist, new versions don't overwrite
- **Deletability** — unlike Arweave, you can clean up old data
- **Programmable metadata** — each blob has a Sui object with agent, timestamp, category
- **Seal integration** — encrypt blobs with access policies before storage

In practice: your vault files live locally for speed, and sync to Walrus periodically for persistence and verification. If you lose your local disk, Walrus has everything.

---

## Seal

[Seal](https://docs.seal.mystenlabs.com) is **decentralized access control** via threshold encryption. Quorum uses it to enforce vault boundaries between agents.

Without Seal, an agent could read any other agent's vault. With Seal:
- Each agent's vault is encrypted with its own Seal policy
- Cross-agent reads require an active proposal in REVIEWING status where the reader is a required reviewer
- The access policy is a Move smart contract — not a config file someone can edit
- Decryption requires cooperation from a threshold of Seal nodes

**Practical implication:** When Bot Analyst reviews a Market Analyst proposal, it can temporarily read the relevant Market Analyst vault files. Once the proposal is no longer in REVIEWING, that access expires automatically.

---

## Sui Layer

The Sui blockchain stores three things:

1. **Proposal objects** — shared objects with the full state machine lifecycle
2. **Agent identities** — owned objects with track records (proposal count, review count)
3. **Audit entries** — lightweight pointers to Walrus blobs with event metadata

Heavy content (proposal text, review feedback, evaluation reports) lives on Walrus. Sui stores metadata and state transitions. This keeps on-chain costs negligible (~5-20 transactions/day, pennies).

**PTB (Programmable Transaction Block):** Sui's mechanism for atomic multi-step operations. When a proposal is approved, one PTB atomically: updates the proposal status, records all reviewer signatures, logs the audit event, and authorizes execution. All succeed or none do.

---

## Context Assembly

The process of building an LLM prompt from an agent's vault, metrics, and task description.

```
CONTEXT.md (always loaded)
+ selected vault knowledge files (by recency and relevance)
+ SQLite query results (recent metrics)
+ task-specific instructions (from YAML prompt_template)
+ proposal content (if reviewing)
= Final prompt (within token budget)
```

The context assembler respects the agent's `context_budget.max_vault_tokens` limit. If the vault has more content than fits, it prioritizes:
1. Files in `always_include`
2. Most recently modified files
3. Files matching the current task's domain

This is why context separation matters. A Market Analyst's 50,000-token budget is entirely devoted to market knowledge. An Engineer's 80,000-token budget is entirely devoted to code and architecture. Neither wastes tokens on the other's domain.

---

## Outcome Evaluation

The **closed feedback loop** that makes agents smarter over time.

After a proposal is executed, the daemon schedules an evaluation (typically 48 hours later). The authoring agent compares its predictions against actual results:

```
Predicted: "Reducing spread to 35 bps will increase fill rate by 15%"
Actual:    "Fill rate increased 22%"
Assessment: "Directionally correct, conservative on magnitude"
```

This evaluation is stored:
- In the agent's vault (`decisions/proposal-X-eval.md`) — so it learns
- On Walrus (full content) — for persistence
- On Sui (hash + metadata) — for verifiability

Over time, each agent builds a **track record**: what fraction of its predictions were directionally correct, how far off the magnitude was, which types of proposals it's best at. This data is on-chain and verifiable by anyone.
