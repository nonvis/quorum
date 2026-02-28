# Quorum — Stakeholders

Who interacts with Quorum, and what does the system look like from their perspective.

---

## Overview

| Stakeholder | Who | Primary Interface | Needs Quorum Software? |
|-------------|-----|-------------------|------------------------|
| **Operator** | Runs the daemon, configures agents, approves proposals | CLI + dashboard | Yes |
| **Observer** | Views decisions and verifies agent track records | Dashboard + Sui explorer | No (on-chain data is self-sufficient) |
| **Developer** | Builds custom agents, integrations, domain templates | YAML + CONTEXT.md + SDK | Yes |
| **Agent** (system) | AI role invoked by daemon | Vault + structured output protocol | N/A (is part of the system) |

---

## Operator

The human who owns the system. Configures agents, starts the daemon, approves human-gated proposals, and tunes agent behavior when output quality drifts.

### Daily Ritual (~15 min at steady state)

```bash
quorum daemon status                     # Healthy?
quorum proposal list --pending           # Anything awaiting approval?
quorum proposal approve --id XXX         # Sign human-gated proposals
quorum audit list --since yesterday      # Skim overnight activity
```

### Key Insight

Operator engagement should **decrease** over time. Early days are high-touch (calibrating CONTEXT.md, reviewing every output). Steady state is low-touch (approve, skim, occasional tune).

### Operator Interface Needs

| Need | Interface | When Available |
|------|-----------|----------------|
| Start/stop daemon | CLI | Phase 1 |
| Approve/reject proposals | CLI → dashboard | Phase 1 → Phase 5 |
| Read vault files | CLI → dashboard | Phase 1 → Phase 5 |
| View agent stats | CLI → dashboard | Phase 1 → Phase 5 |
| Notification of human-gated proposals | TBD | TBD |
| Emergency pause/resume | CLI | Phase 1 |
| Cost monitoring | Dashboard | Phase 5 |

### Open Questions

1. **Mobile approval:** Can operator approve from phone? (Sui wallet on mobile?)
2. **Vacation mode:** Auto-approve low-risk proposals when operator is away?
3. **Operator handoff:** Shared wallet or multi-sig for multiple operators?
4. **Alert fatigue:** How to notify without over-notifying?

---

## Observer

Doesn't operate Quorum — **verifies** it. Looks at decisions after the fact and confirms the system operated correctly.

### Who

- Auditor reviewing compliance
- Investor evaluating a Quorum-managed strategy
- Grant reviewer assessing real usage
- Counterparty verifying governance

### What They See (No Quorum Software Required)

Everything on Sui blockchain:

```
Proposal Objects     → state transitions, reviewer verdicts, timestamps, human signatures
Agent Identity       → role, registration, proposal/review counts, track record
Audit Log Entries    → event type, agent ID, timestamp, Walrus blob ID, content hash
```

Full content (proposal text, reviews, evaluations) on Walrus, verifiable against on-chain hashes.

### Verification Flow

```
1. Get Sui package ID from operator
2. Query proposal history on Sui explorer
3. Check: approval rate, human involvement, track records
4. Spot-check: fetch 3 proposals from Walrus, read full text, verify hashes
5. Conclusion: transparent operation with human oversight
```

### Design Implications

1. **Dashboard needs a read-only / public mode** — no operator credentials required
2. **On-chain data must be self-descriptive** — readable without documentation
3. **Track record computation must be deterministic** — same data → same accuracy, regardless of who computes
4. **Walrus blob format should be documented** — observer tools can parse independently

---

## Developer

Builds **on** Quorum — custom agents, domain templates, integrations, or embeds Quorum into a larger product.

### What They Create

Three files define an agent:

1. **Agent YAML** — schedule, triggers, inference tier, context budget, boundaries
2. **Vault CONTEXT.md** — agent identity, knowledge scope, what it does and doesn't do
3. **Task YAML** — inputs (SQLite, vault, HTTP), outputs (vault_update, proposal), prompt template

### Developer Mental Model

```
I write CONTEXT.md (who the agent is)
I write YAML (when and how the agent runs)
I write prompt templates (what the agent thinks about)
I feed data into SQLite (what the agent sees)
Quorum handles everything else: scheduling, consensus, storage, audit
```

### Key Interfaces

| Need | Interface | When Available |
|------|-----------|----------------|
| Define agents | YAML + CONTEXT.md | Phase 1 |
| Test agents locally | CLI (`quorum agent invoke`) | Phase 1 |
| Programmatic access | TypeScript SDK | Phase 5 |
| Event subscriptions | TS SDK (WebSocket) | Phase 5 |
| Domain templates | Template packaging system | Phase 5 |

### Open Questions

1. **Agent testing harness:** Mock context + dry-run invocation without full daemon?
2. **Prompt iteration workflow:** Edit CONTEXT.md → invoke → review output → repeat. Needs fast loop.
3. **Plugin system:** Custom output block types beyond VAULT_UPDATE/PROPOSAL/REVIEW/SUMMARY?
4. **Vault schema validation:** Enforce structure or stay free-form markdown?
5. **Multi-language SDK:** Python for data science teams? Or TypeScript-only?

---

## Agent (System Stakeholder)

The AI role invoked by the daemon. Has no continuity — each invocation is a fresh LLM call.

### What It Experiences

```
WAKE UP → read vault (identity + knowledge) → read fresh data → think → write output → CEASE
```

### Its Only Persistence Mechanism

Writing to its vault via VAULT_UPDATE blocks. If it doesn't write, it forgets.

### Cross-Agent Interaction

Agents never talk directly. All interaction is mediated by the proposal protocol:
- Author creates proposal
- Reviewers evaluate via consensus engine
- Author revises if requested
- Cross-vault reads only during active review (Seal-authorized)

### Growth Trajectory

The vault IS the agent's value. A week-old agent with thin vault ≈ generic LLM. A month-old agent with rich vault ≈ domain expert with institutional memory.

---

## Design Principles (Cross-Stakeholder)

1. **Operator engagement decreases over time** — system becomes more autonomous as track records build
2. **Observer needs zero trust** — verification through on-chain data, not through the operator or software
3. **Developer defines three files** — everything else is framework responsibility
4. **CONTEXT.md is the highest-leverage file** — more impactful than any code change in the system
5. **Vault is the moat** — accumulated knowledge is what makes each Quorum instance valuable over time
