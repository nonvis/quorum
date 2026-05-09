# Quorum — Domain Templates

Same daemon, different teams. This doc shows concrete team compositions for different domains using the six agent archetypes.

---

## The Pattern

Every domain needs at minimum a **leader** and one specialist. The six archetypes determine what an agent can do; the CONTEXT.md and SKILL.md determine what it knows.

| Archetype | Purpose | Tool Access | Required? |
|-----------|---------|-------------|-----------|
| **leader** | Receives user prompt, coordinates team, decides when done | Read-only | Exactly 1 |
| **thinker** | Plans, designs, strategizes | Read-only | 1+ recommended |
| **doer** | Writes code, runs commands, produces artifacts | Full tools | 1+ recommended |
| **reviewer** | Validates work, catches bugs, enforces standards | Read-only | Optional |
| **scribe** | Consumes the conversation transcript, writes project notes (Obsidian) | Write to notes dir | Optional |
| **librarian** | Consumes the conversation transcript, writes human-facing docs | Write to docs dir | Optional |

**The key insight:** Role determines tool access. Specialization determines domain expertise. A `move-dev` doer and a `ts-dev` doer have the same tool permissions — full read/write/execute. What makes them different agents is their CONTEXT.md (role instructions) and SKILL.md (domain knowledge). Same archetype, different skill files, completely different agent.

---

## Trading Operations

A team for optimizing a market-making bot. The leader coordinates analysis cycles, thinkers analyze markets and parameters, the doer implements config changes, and the scribe records strategy decisions.

```yaml
conversations:
  leader: leader
  max_turns: 20
  default_path: [leader, defi-strategist, parameter-optimizer, move-dev, strategy-scribe]
  agents: [leader, defi-strategist, parameter-optimizer, move-dev, strategy-scribe]
```

| Agent ID | Archetype | Description |
|----------|-----------|-------------|
| `leader` | leader | Coordinates analysis cycles, decides when to act vs. observe |
| `defi-strategist` | thinker | Analyzes pool metrics, competitor spreads, volume patterns, macro signals |
| `parameter-optimizer` | thinker | Reviews P&L, fill rates, adverse selection — recommends parameter changes |
| `move-dev` | doer | Implements config changes, writes Move modules, runs backtests |
| `strategy-scribe` | scribe | Records strategy decisions, parameter change history, performance outcomes |

**Example goal:** "Analyze DEEP/SUI pool performance over the last 7 days and recommend spread adjustments."

**Flow:** Leader hands to defi-strategist (market analysis) then parameter-optimizer (parameter recommendation) then move-dev (implement config change) then strategy-scribe (record decision).

---

## Software Engineering

A team for building and maintaining a codebase. Two doers with different specializations — one for TypeScript, one for Move — plus a security reviewer to catch vulnerabilities before merge.

```yaml
conversations:
  leader: leader
  max_turns: 25
  default_path: [leader, architect, ts-dev, scribe]
  agents: [leader, architect, ts-dev, move-dev, security-reviewer, api-scribe]
```

| Agent ID | Archetype | Description |
|----------|-----------|-------------|
| `leader` | leader | Routes work to the right developer, manages multi-step implementations |
| `architect` | thinker | Designs module structure, API contracts, data models |
| `ts-dev` | doer | Writes TypeScript — backend services, API endpoints, tests |
| `move-dev` | doer | Writes Move smart contracts, modules, and tests |
| `security-reviewer` | reviewer | Reviews code for vulnerabilities, access control bugs, unsafe patterns |
| `api-scribe` | scribe | Records API design decisions, architecture notes, change rationale |

**Example goal:** "Add a new REST endpoint for querying escrow status by address."

**Flow:** Leader hands to architect (API design) then ts-dev (implement endpoint) then security-reviewer (validate) then api-scribe (document).

**Multiple doers:** The leader decides which doer to hand off to based on the task. Move work goes to `move-dev`, TypeScript work to `ts-dev`. For cross-stack features, the leader chains them: architect designs, then move-dev implements the contract, then ts-dev implements the API layer.

---

## Infrastructure / DevOps

A team for managing servers, deployments, and operational health. Smaller team — the infra-planner combines strategic and tactical thinking.

```yaml
conversations:
  leader: leader
  max_turns: 15
  default_path: [leader, infra-planner, ops-doer, architecture-scribe]
  agents: [leader, infra-planner, ops-doer, architecture-scribe]
```

| Agent ID | Archetype | Description |
|----------|-----------|-------------|
| `leader` | leader | Triages operational issues, prioritizes work |
| `infra-planner` | thinker | Plans capacity, designs network topology, evaluates tooling |
| `ops-doer` | doer | Writes configs, runs deployments, executes maintenance tasks |
| `architecture-scribe` | scribe | Records infrastructure decisions, runbooks, topology changes |

**Example goal:** "Our fullnode is 200 blocks behind. Diagnose and fix."

**Flow:** Leader hands to infra-planner (diagnose root cause) then ops-doer (implement fix — adjust configs, restart services) then architecture-scribe (record incident and resolution).

---

## Research

A team for running experiments and synthesizing findings. The methods-reviewer ensures statistical rigor before conclusions are recorded.

```yaml
conversations:
  leader: leader
  max_turns: 20
  default_path: [leader, literature-reviewer, experiment-coder, methods-reviewer, findings-scribe]
  agents: [leader, literature-reviewer, experiment-coder, methods-reviewer, findings-scribe]
```

| Agent ID | Archetype | Description |
|----------|-----------|-------------|
| `leader` | leader | Frames research questions, coordinates investigation cycles |
| `literature-reviewer` | thinker | Reviews prior work, identifies gaps, proposes hypotheses |
| `experiment-coder` | doer | Implements experiments, runs analysis scripts, produces visualizations |
| `methods-reviewer` | reviewer | Validates statistical methods, checks for confounds, reviews significance |
| `findings-scribe` | scribe | Synthesizes findings into structured research notes |

**Example goal:** "Test whether our fill rate improves with asymmetric spread skew during high-volatility regimes."

**Flow:** Leader hands to literature-reviewer (prior work on skew strategies) then experiment-coder (implement backtest, run analysis) then methods-reviewer (validate methodology and results) then findings-scribe (record findings).

---

## Scaling: Team Sizes

### Minimum Viable Team (2 agents)

```yaml
agents: [leader, doer]
default_path: [leader, doer]
```

Leader receives the goal, hands to doer, doer does the work, hands back to leader, leader marks done. No planning step, no review, no documentation. Good for simple tasks.

### Standard Team (4 agents)

```yaml
agents: [leader, thinker, doer, scribe]
default_path: [leader, thinker, doer, scribe]
```

Leader coordinates, thinker plans, doer executes, scribe documents. The workhorse configuration for most projects.

### Full Team (6+ agents)

```yaml
agents: [leader, thinker-a, thinker-b, doer-a, doer-b, reviewer, scribe, librarian]
default_path: [leader, thinker-a, doer-a, reviewer, scribe]
```

Multiple thinkers for different planning concerns (e.g., architecture + security). Multiple doers for different tech stacks (e.g., Move + TypeScript). Reviewer for quality gates. Scribe for internal notes, librarian for external docs.

The `default_path` defines the happy path. The leader can override routing at any turn via HANDOFF blocks — sending work to `thinker-b` instead of `thinker-a`, or skipping the reviewer for trivial changes.

---

## The Specialization Model

Two agents with the same archetype differ only in their context and skill files:

```
move-dev (doer)                         ts-dev (doer)
  CONTEXT.md: from templates/agents/      CONTEXT.md: from templates/agents/
    move-dev.md                             ts-dev.md
  Role skill: quorum-roles/doer           Role skill: quorum-roles/doer
  Domain skill: sui-move +                Domain skill: sui-ts-sdk
    move-code-quality
  target_dir: ~/my-move-project           target_dir: ~/my-ts-project
```

Both have full tool access (read, write, execute). Both receive HANDOFF prompts the same way. The daemon treats them identically — it routes by agent ID, not by what they know.

This means adding a new specialization is always the same steps:

1. `quorum agent create --role doer --name python-dev --no-ai` (uses `templates/agents/doer.md`, auto-detects role skill)
2. Edit `.quorum/vaults/python-dev/CONTEXT.md` if needed
3. Optionally install a domain skill to `~/.claude/skills/` and set `skill_file` in the agent YAML
4. Restart daemon

---

## Domain Mapping Table

How the six archetypes map across domains:

| Archetype | Trading | Software | Infrastructure | Research |
|-----------|---------|----------|----------------|----------|
| **leader** | Coordinator | Coordinator | Coordinator | Coordinator |
| **thinker** | DeFi Strategist, Parameter Optimizer | Architect | Infra Planner | Literature Reviewer |
| **doer** | Move Developer | TS Dev, Move Dev | Ops Doer | Experiment Coder |
| **reviewer** | — | Security Reviewer | — | Methods Reviewer |
| **scribe** | Strategy Scribe | API Scribe | Architecture Scribe | Findings Scribe |
| **librarian** | — | API Doc Writer | Runbook Writer | Paper Drafter |

The leader is always the same archetype. Everything else is a configuration choice — which archetypes to include and what CONTEXT.md to give them.

Quorum is horizontal infrastructure. The six archetypes are the platform; domains are configurations.
