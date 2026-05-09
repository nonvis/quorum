# Quorum — Core Concepts

A glossary of everything you need to understand to use Quorum effectively.

---

## Agent

An agent is a **role, not a process**. It doesn't run continuously — the daemon invokes it via a `claude -p` subprocess when the ball reaches it. Each invocation is episodic: context in, structured output out, process exits.

Quorum defines **6 archetypes** that determine what an agent can do:

| Role | Count | Tool Access | Purpose |
|------|-------|-------------|---------|
| **leader** | exactly 1 | analyst | Receives user prompt, decomposes work, coordinates team, triggers scribe at end |
| **thinker** | 1+ | analyst | Plans, designs, architects |
| **doer** | 1+ | executor (full tools) | Executes code changes in target repo |
| **reviewer** | 0+ | analyst | Validates work. Optional. |
| **scribe** | 0+ | analyst | Consumes the conversation transcript and produces project notes for agents |
| **librarian** | 0+ | analyst | Consumes the conversation transcript and produces human-facing docs |

Role determines tool access. Doers get executor-class (`claude -p` with full tools). All others are analyst-class (`--disallowedTools "Write,Edit,NotebookEdit"`).

An agent is defined by:
- **YAML config** (`.quorum/agents/agent.yaml`) — role, description, vault path, skill file, optional model override
- **Vault CONTEXT.md** — who it is, what it knows, what it does and doesn't do (generated from `templates/agents/{role}.md`, includes Universal Rules for HANDOFF discipline)
- **SKILL.md** — behavioral patterns for the role (auto-detected from `~/.claude/skills/quorum-roles/{role}/SKILL.md`)
- **Optional domain SKILL.md** — specialized expertise (e.g., sui-move, sui-ts-sdk)
- **Optional `model`** — per-agent model override (e.g., `sonnet`, `opus`). When set, adds `--model` to `claude -p`. Useful for running cheap agents on sonnet and quality-critical agents on opus.

Between invocations, an agent has no state. Everything it "remembers" comes from its vault, which the daemon loads into the LLM prompt.

**Key insight:** Agents are cheap to define and expensive to run well. A bad CONTEXT.md produces bad outputs regardless of model quality. Writing good agent instructions is the primary skill.

---

## Vault

A vault is an **agent's working memory on disk** — a folder of files that grows over time.

```
.quorum/vaults/{agent}/
├── CONTEXT.md          # Always loaded (role identity, from templates/agents/{role}.md)
├── knowledge/          # Accumulated findings, scribe-produced notes
```

SKILL.md files live in `~/.claude/skills/` (installed via `scripts/install-skills.sh`) and are referenced by path in the agent YAML's `skill_file` field.

The daemon's context assembler selects which vault files to include in each invocation, staying within the agent's token budget. CONTEXT.md always loads. Other files are selected by recency and relevance.

Vaults are local filesystem only. Plain files you can read, edit, or seed manually.

**What makes vaults different from other agent memory:**
- They're files, not embeddings — you can read them, version them, diff them
- Scribe-produced notes feed back into vaults, creating a growth loop
- The team roster (list of all agents) is injected into CONTEXT.md from config

---

## HANDOFF Protocol

The ball-passing mechanism that moves control between agents. One ball, always moving. Only one agent is active at any time (sequential dispatch).

An agent yields control by including a HANDOFF block in its output:

```
```HANDOFF
to: <agent_id | human | done>
prompt: <instructions for the next agent>
```
```

**Routing priority:**
1. HANDOFF block present → route to specified agent (path_index synced if target is in default_path)
2. No HANDOFF, `default_path` configured → next in path (path_index incremented)
3. No HANDOFF, no `default_path` → conversation done
4. Unknown agent → fallback to leader

**Universal Rules:** All agent templates enforce HANDOFF discipline via a `## Universal Rules` section: never self-HANDOFF, always SUMMARY before HANDOFF, preserve "Task N:" prefix through the chain, and role-specific routing (doers/reviewers -> scribe, scribe -> done, leader -> architect/thinker).

**Special values:**
- `human` — leader holds the ball and waits for operator input (`waiting_for_human` state)
- `done` — cycle complete, conversation moves to `done` state

The human only interacts with the leader. When user input is needed, the leader holds the ball.

---

## Daemon

A **deterministic C++20 process** that orchestrates agent invocations. It has three components:

| Component | What It Does |
|-----------|-------------|
| **ConversationEngine** | Manages conversation state, dispatches agents, processes HANDOFF blocks |
| **Scheduler** | Fires agent invocations based on ball-passing and default paths |
| **Budget Enforcer** | Tracks token/cost usage, enforces window budget and per-conversation max turns |

**Critical property:** The daemon never calls an LLM. All routing, dispatching, and state management is pure compiled code. LLM calls happen only inside agent subprocesses (`claude -p`), which the daemon spawns but does not participate in.

This means:
- If the LLM provider is down, the daemon keeps running (just can't invoke agents)
- Daemon behavior is reproducible and debuggable — no "the AI decided to skip this step"
- Sequential dispatch: one agent at a time, no concurrency within a cycle

**Conversation states:** `active`, `waiting_for_human`, `done`, `closed`, `paused`

---

## Context Assembly

The process of building an LLM prompt from an agent's vault and task description.

```
CONTEXT.md (always loaded)
+ SKILL.md (if present)
+ selected vault knowledge files (by recency and relevance)
+ team roster (injected from config)
+ task-specific instructions (HANDOFF prompt from previous agent)
= Final prompt (within token budget)
```

Context separation matters. Each agent's budget is entirely devoted to its domain. A thinker's context is plans and architecture. A doer's context is code and implementation details. Neither wastes tokens on the other's domain.

---

## Team Roster

The list of all agents in the current configuration, injected into each agent's context so it knows who it can hand off to.

Defined in the conversation config:

```yaml
conversations:
  leader: leader
  max_turns: 20
  default_path: [leader, thinker, doer, scribe]  # optional
  agents: [leader, thinker, doer, reviewer, scribe]
```

Each agent sees the roster and can address HANDOFF blocks to any listed agent by ID.

---

## Scribe & Librarian

Two archetypes that consume the conversation transcript at the end of a cycle. Same mechanism, different audiences:

- **Scribe** — produces structured notes for agent consumption. Output feeds back into agent vaults, building institutional memory over time.
- **Librarian** — produces human-facing documentation. Output is formatted for the operator or developer to read.

Both are analyst-class (no write access to code). Both read the full conversation transcript via `claude -p` session resume.

The leader typically triggers the scribe as the last step before ending a cycle.

---

## Agent Knowledge Growth

Agents start empty and become valuable over time:

```
Week 1:  CONTEXT.md only (identity, no experience)
         Output quality: low — generic, cautious
         Vault: ~2 files

Week 4:  CONTEXT.md + 10-15 knowledge files
         Output quality: moderate — domain-aware, references history
         Vault: ~15 files, scribe notes accumulating

Week 12: CONTEXT.md + 30-50 knowledge files
         Output quality: high — pattern-aware, calibrated judgment
         Vault: ~50 files, rich institutional memory
```

The growth loop: agents work through conversation cycles, the scribe distills each cycle into vault notes, those notes load into future invocations, producing better output over time.

The vault is the agent's institutional memory. An agent with a rich vault produces dramatically better output than a fresh agent, even with the same CONTEXT.md and the same model.
