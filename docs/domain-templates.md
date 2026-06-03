# Quorum — Domain Roster Examples

Concrete agent rosters for different domains. Same seven archetypes, different specializations via CONTEXT.md + SKILL.md.

There is no team layer — you provision a set of agents and the leader routes each goal to the best-fit agents across the full roster. Each example below is the **roster you'd provision** for that kind of work, plus an example goal you'd hand to `quorum converse`.

For the "why" (what each archetype does, why specialization works, etc.) — see the project's design vault. This file is **examples only**.

---

## Trading Operations

Optimizing a market-making bot. Leader coordinates analysis cycles; thinkers analyze markets and parameters; doer implements config changes; scribe records strategy decisions.

Provision these agents:
- `leader` (leader) — coordinates cycles, decides when to act vs observe
- `defi-strategist` (thinker) — pool metrics, competitor spreads, macro signals
- `parameter-optimizer` (thinker) — P&L, fill rates, adverse selection
- `move-dev` (doer) — implements config changes, runs backtests
- `strategy-scribe` (scribe) — records decisions, parameter history, outcomes

```bash
quorum agent create --role thinker --name defi-strategist --no-ai
quorum agent create --role thinker --name parameter-optimizer --no-ai
quorum agent create --role doer --name move-dev --target-dir . --no-ai
quorum agent create --role scribe --name strategy-scribe --no-ai
```

**Example goal:** `quorum converse "Analyze DEEP/SUI pool performance over the last 7 days and recommend spread adjustments."`

---

## Software Engineering

Building/maintaining a codebase. Two doers with different specializations (TypeScript, Move) plus a `thinker`-role security review specialty.

Provision these agents:
- `leader` (leader) — routes work, manages multi-step implementations
- `architect` (thinker) — module structure, API contracts, data models
- `ts-dev` (doer) — TypeScript backend, APIs, tests
- `move-dev` (doer) — Move contracts and modules
- `security-reviewer` (thinker) — vulnerabilities, access control, unsafe patterns (a `thinker`-role review specialty)
- `api-scribe` (scribe) — API design decisions, architecture notes

```bash
quorum agent create --role thinker --name architect --no-ai
quorum agent create --role doer --name ts-dev --target-dir . --no-ai
quorum agent create --role doer --name move-dev --target-dir . --no-ai
quorum agent create --role thinker --name security-reviewer --no-ai
quorum agent create --role scribe --name api-scribe --no-ai
```

**Example goal:** `quorum converse "Add a REST endpoint for querying escrow status by address."`

Multiple doers: the leader picks based on the task. Cross-stack features chain them.

---

## Infrastructure / DevOps

Server/deployment/operational health. Smaller roster — `infra-planner` covers both strategic and tactical thinking.

Provision these agents:
- `leader` (leader) — triages issues, prioritizes work
- `infra-planner` (thinker) — capacity, topology, tooling
- `ops-doer` (doer) — configs, deployments, maintenance
- `architecture-scribe` (scribe) — runbooks, topology changes

```bash
quorum agent create --role thinker --name infra-planner --no-ai
quorum agent create --role doer --name ops-doer --target-dir . --no-ai
quorum agent create --role scribe --name architecture-scribe --no-ai
```

**Example goal:** `quorum converse "Our fullnode is 200 blocks behind. Diagnose and fix."`

---

## Research

Experiments + synthesis. A `thinker`-role methods review specialty ensures statistical rigor.

Provision these agents:
- `leader` (leader) — frames questions, coordinates cycles
- `literature-reviewer` (thinker) — prior work, hypotheses
- `experiment-coder` (doer) — implements experiments, analysis, visualizations
- `methods-reviewer` (thinker) — statistical methods, confounds, significance (a `thinker`-role review specialty)
- `findings-scribe` (scribe) — research notes

```bash
quorum agent create --role thinker --name literature-reviewer --no-ai
quorum agent create --role doer --name experiment-coder --target-dir . --no-ai
quorum agent create --role thinker --name methods-reviewer --no-ai
quorum agent create --role scribe --name findings-scribe --no-ai
```

**Example goal:** `quorum converse "Test whether fill rate improves with asymmetric spread skew during high-volatility regimes."`

---

## Brainstorming variant

Any roster above can run a goal in `--mode brainstorm` for read-only exploration. Same agents, but project files are untouched and the scribe distributes curated knowledge across agent vaults instead of writing project notes. See vault `12 - Execution Modes.md` for mode details.

---

An `evaluator` can join any build roster to score doer output against a specialty rubric ("is it *good*?"); correctness/convention checks ("does it work?") are handled by a `thinker`-role review specialty or the doer itself.

## Adding a domain

1. Create the agents the work needs via `quorum agent create --role <r> --name <n>` (`quorum init` already provisions the full default roster, so you're only adding the specialized extras)
2. Each agent's CONTEXT.md (auto-generated from the role template) defines what it does — tune it via the web UI's CONTEXT.md editor
3. Optional: assign a `skill_file` per agent for domain expertise (e.g., `~/.claude/skills/sui-dev-skills/sui-move`)
4. Run `quorum converse "<goal>"` — the leader routes the goal to the best-fit agents across the roster (no team to select)

For the end-to-end setup + operation walkthrough, see the design vault's `99 - Quorum Manual.md`.

The daemon, conversation engine, HANDOFF protocol, and tool sandboxing are domain-agnostic. Only the agents change.
