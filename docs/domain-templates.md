# Quorum — Domain Team Examples

Concrete team configurations for different domains. Same six archetypes, different specializations via CONTEXT.md + SKILL.md.

For the "why" (what each archetype does, why specialization works, etc.) — see the project's design vault. This file is **examples only**.

---

## Trading Operations

Optimizing a market-making bot. Leader coordinates analysis cycles; thinkers analyze markets and parameters; doer implements config changes; scribe records strategy decisions.

```yaml
# .quorum/teams/trading.yaml
name: Trading
default_path: [leader, defi-strategist, parameter-optimizer, move-dev, strategy-scribe]
```

Agents:
- `leader` (leader) — coordinates cycles, decides when to act vs observe
- `defi-strategist` (thinker) — pool metrics, competitor spreads, macro signals
- `parameter-optimizer` (thinker) — P&L, fill rates, adverse selection
- `move-dev` (doer) — implements config changes, runs backtests
- `strategy-scribe` (scribe) — records decisions, parameter history, outcomes

**Example goal:** "Analyze DEEP/SUI pool performance over the last 7 days and recommend spread adjustments."

---

## Software Engineering

Building/maintaining a codebase. Two doers with different specializations (TypeScript, Move) plus security reviewer.

```yaml
# .quorum/teams/eng.yaml
name: Engineering
default_path: [leader, architect, ts-dev, scribe]
```

Agents:
- `leader` (leader) — routes work, manages multi-step implementations
- `architect` (thinker) — module structure, API contracts, data models
- `ts-dev` (doer) — TypeScript backend, APIs, tests
- `move-dev` (doer) — Move contracts and modules
- `security-reviewer` (reviewer) — vulnerabilities, access control, unsafe patterns
- `api-scribe` (scribe) — API design decisions, architecture notes

**Example goal:** "Add a REST endpoint for querying escrow status by address."

Multiple doers: leader picks based on task. Cross-stack features chain them.

---

## Infrastructure / DevOps

Server/deployment/operational health. Smaller team — `infra-planner` covers both strategic and tactical thinking.

```yaml
# .quorum/teams/infra.yaml
name: Infra
default_path: [leader, infra-planner, ops-doer, architecture-scribe]
```

Agents:
- `leader` (leader) — triages issues, prioritizes work
- `infra-planner` (thinker) — capacity, topology, tooling
- `ops-doer` (doer) — configs, deployments, maintenance
- `architecture-scribe` (scribe) — runbooks, topology changes

**Example goal:** "Our fullnode is 200 blocks behind. Diagnose and fix."

---

## Research

Experiments + synthesis. Methods-reviewer ensures statistical rigor.

```yaml
# .quorum/teams/research.yaml
name: Research
default_path: [leader, literature-reviewer, experiment-coder, methods-reviewer, findings-scribe]
```

Agents:
- `leader` (leader) — frames questions, coordinates cycles
- `literature-reviewer` (thinker) — prior work, hypotheses
- `experiment-coder` (doer) — implements experiments, analysis, visualizations
- `methods-reviewer` (reviewer) — statistical methods, confounds, significance
- `findings-scribe` (scribe) — research notes

**Example goal:** "Test whether fill rate improves with asymmetric spread skew during high-volatility regimes."

---

## Brainstorming variant

Any of the above teams can run in `--mode brainstorm` for read-only exploration. Same agents, but project files are untouched and the scribe distributes curated knowledge across team vaults instead of writing project notes. See vault `12 - Execution Modes.md` for mode details.

---

## Adding a domain

1. Create `.quorum/teams/<name>.yaml` with `name` and `default_path`
2. Create the agents the team references via `quorum agent create --role <r> --name <n>`
3. Each agent's CONTEXT.md (auto-generated from role template) defines what it does
4. Optional: assign a `skill_file` per agent for domain expertise (e.g., `~/.claude/skills/sui-dev-skills/sui-move`)
5. Run: `quorum converse --team <name> "<goal>"`

The daemon, conversation engine, HANDOFF protocol, and tool sandboxing are domain-agnostic. Only the agents change.
