# Quorum — Stakeholders

Who interacts with Quorum, and what does the system look like from their perspective.

---

## Overview

| Stakeholder | Who | Primary Interface |
|-------------|-----|-------------------|
| **Operator** | Runs the daemon, configures agents, responds to leader | CLI |
| **Developer** | Defines agents via YAML + CONTEXT.md | Config files + `quorum agent create` |
| **Agent** (system) | AI role invoked by daemon via `claude -p` | Vault + HANDOFF protocol |

---

## Operator

The human who owns the system. Starts the daemon, responds to the leader when it holds the ball (`waiting_for_human`), and tunes agent behavior when output quality drifts.

### Daily Ritual (~5 min at steady state)

```bash
quorum status                    # Daemon healthy? Any conversations active?
quorum conversations             # Check for waiting_for_human state
quorum respond "..."             # Answer leader's question if waiting
```

No proposal approval workflow. No on-chain signing. The operator's job is to give the leader direction and occasionally refine agent configs.

### Engagement Over Time

Early days are high-touch: calibrating CONTEXT.md, reviewing every output, correcting agent behavior. Steady state is low-touch: respond to leader queries, skim scribe output, occasional config tune.

The goal is for the operator to interact less over time as agents accumulate knowledge and the team becomes more self-sufficient.

---

## Developer

Builds agents — either manually or using the `quorum agent create` generator, which interviews the user and produces config files.

### What They Create

Three things define an agent:

1. **Agent YAML** (`.quorum/agents/agent.yaml`) — role (one of 6 archetypes), description, vault path, skill file
2. **CONTEXT.md** (`.quorum/vaults/{agent}/CONTEXT.md`) — agent identity, from `templates/agents/{role}.md`
3. **SKILL.md** (auto-detected from `~/.claude/skills/quorum-roles/{role}/SKILL.md`) — behavioral patterns per role

Most of this is generated automatically by `quorum agent create --no-ai`.

### Developer Mental Model

```
I write CONTEXT.md (who the agent is)
I write YAML (what role it plays, where its vault lives)
I optionally write SKILL.md (domain expertise)
Quorum handles everything else: dispatching, ball-passing, budget
```

### The Generator Shortcut

`quorum agent create` scaffolds the YAML config, CONTEXT.md, and vault directory. In `--no-ai` mode it uses role-specific templates from `templates/agents/` with placeholder substitution. Without `--no-ai`, it uses Claude Code to generate a richer CONTEXT.md. Role skills are auto-detected and set in the YAML automatically.

### Key Insight

CONTEXT.md is the highest-leverage file. A well-written CONTEXT.md with a mediocre model outperforms a poorly-written CONTEXT.md with a frontier model. The developer's primary skill is writing clear, specific agent instructions.

---

## Agent (System Stakeholder)

The AI role invoked by the daemon. Has no continuity — each invocation is a fresh `claude -p` subprocess. One session per cycle.

### What It Experiences

```
SPAWN → read vault (identity + knowledge) → read task prompt → think → write output (HANDOFF) → EXIT
```

### Its Persistence Mechanisms

Two ways an agent's work survives beyond its invocation:
1. **Scribe-distilled vault notes** — written at end of cycle, loaded into future invocations
2. **Vault writes** (doer only) — direct file changes in the target repo

### Cross-Agent Coordination

Agents never talk directly. All coordination flows through HANDOFF blocks:
- Agent A finishes work, includes `HANDOFF to: agent_b` with instructions
- Daemon spawns Agent B with those instructions as the task prompt
- Agent B does its work, hands off to the next agent or back to leader

The leader orchestrates the sequence. Individual agents focus on their specialty.

### Growth Trajectory

The vault IS the agent's value. A week-old agent with a thin vault is a generic LLM with a role label. A month-old agent with a rich vault — fed by scribe-distilled notes from dozens of cycles — is a domain expert with institutional memory.

---

## Design Principles

1. **Operator engagement decreases over time** — the system becomes more self-sufficient as agent vaults accumulate knowledge
2. **Developer defines agent configs — everything else is framework responsibility** — no need to understand daemon internals to build effective agents
3. **CONTEXT.md is the highest-leverage file** — more impactful than any code change in the system
4. **Vault is the moat** — accumulated knowledge is what makes each Quorum instance valuable over time
