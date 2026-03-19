# Quorum — End-to-End Flow

> What it looks like when everything works.

This document walks through the complete Quorum experience from a cold start to a running multi-agent team. The user has a Claude Pro/Max subscription (for `claude` CLI) and wants to orchestrate a team of AI agents that coordinate, accumulate knowledge, and produce artifacts.

---

## The 30-Second Mental Model

```
You define a team → Start a conversation with a goal → Leader receives goal
→ Agents pass the ball via HANDOFF blocks → Knowledge accumulates in a ledger
→ Scribe writes notes at end → Done (or leader asks human for input)
```

The daemon is a deterministic C++20 process. Agents are `claude -p` subprocesses — stateless LLM calls that read context, think, and respond with structured blocks. One ball, always moving. One agent runs at a time. The daemon never calls an LLM itself; all intelligence lives in the agent layer.

---

## Setup (~5 minutes)

### Build from Source

```bash
git clone https://github.com/user/quorum && cd quorum

# macOS dependencies
brew install openssl sqlite

# Build
mkdir build && cd build
cmake .. && make -j$(nproc)

# Verify
./quorum_daemon --help
```

### Prerequisites

- **Claude CLI** — `claude` must be on your PATH. Install via `npm install -g @anthropic-ai/claude-code`.
- **SQLite** — ships with macOS. Linux: `apt install libsqlite3-dev`.
- **OpenSSL** — `brew install openssl` (macOS) or `apt install libssl-dev` (Linux).

No API keys to configure. Quorum spawns `claude -p` subprocesses, which use your existing Claude CLI authentication.

---

## Define Your Team

A Quorum project is a YAML config that declares a leader and a set of agents. Each agent gets a YAML file, a CONTEXT.md (role instructions), and optionally a SKILL.md (domain expertise).

### Project Config

```yaml
# .quorum/config.yaml (created by `quorum init`)
daemon:
  data_dir: .quorum
  pid_file: .quorum/quorum.pid
  log_level: info

conversations:
  enabled: true
  leader: leader
  default_max_rounds: 20
  default_budget_usd: 5.0

budget:
  window_budget_usd: 100.00
  window_hours: 5
```

### Agent YAMLs

Each agent declares its role, archetype, and context paths:

```yaml
# .quorum/agents/move-dev.yaml
id: move-dev
name: "Move Developer"
role: doer
agent_class: executor
description: "Writes Move smart contracts"
vault_path: .quorum/vaults/move-dev/
context_file: .quorum/vaults/move-dev/CONTEXT.md
skill_file: ~/.claude/skills/quorum-roles/doer/SKILL.md  # auto-detected from role
executor:
  target_dir: ~/nonvis/my-move-project
```

```yaml
# .quorum/agents/architect.yaml
id: architect
name: "Architect"
role: thinker
agent_class: analyst
description: "Plans module structure, designs APIs, reviews patterns"
vault_path: .quorum/vaults/architect/
context_file: .quorum/vaults/architect/CONTEXT.md
skill_file: ~/.claude/skills/quorum-roles/thinker/SKILL.md  # auto-detected from role
```

```yaml
# .quorum/agents/security-reviewer.yaml
id: security-reviewer
name: "Security Reviewer"
role: reviewer
agent_class: analyst
description: "Validates Move code for vulnerabilities and access-control bugs"
vault_path: .quorum/vaults/security-reviewer/
context_file: .quorum/vaults/security-reviewer/CONTEXT.md
skill_file: ~/.claude/skills/quorum-roles/reviewer/SKILL.md  # auto-detected from role
```

### CONTEXT.md — Who the Agent Is

Every agent loads its CONTEXT.md into every invocation. This is the agent's identity:

```markdown
# Move Developer — Agent Context

## Role
You implement Move smart contracts on Sui. You write production-quality code,
compile it, run tests, and fix issues until tests pass.

## You Produce
- Move modules in sources/
- Move tests in tests/
- KNOWLEDGE blocks documenting design decisions

## You Do NOT
- Plan architecture (that's the architect)
- Review your own code (that's the security reviewer)
- Write documentation (that's the scribe)
```

### Agent Generator

Instead of hand-writing agent configs, use the generator:

```bash
# AI-assisted (uses claude -p to generate CONTEXT.md)
quorum agent create --role doer --name move-dev --target-dir ~/my-project

# Template-based (uses templates/agents/doer.md with placeholder substitution)
quorum agent create --role doer --name move-dev --target-dir ~/my-project --no-ai
```

This scaffolds the YAML config, CONTEXT.md, and vault directory. Role skills are auto-detected from `~/.claude/skills/quorum-roles/{role}/SKILL.md` (install via `scripts/install-skills.sh`). Specialized templates exist for `move-dev` and `ts-dev` roles.

### Example Team: Move Development

```
my-move-project/
├── .quorum/
│   ├── config.yaml
│   ├── quorum.db
│   ├── agents/
│   │   ├── leader.yaml
│   │   ├── architect.yaml
│   │   ├── move-dev.yaml
│   │   ├── security-reviewer.yaml
│   │   └── scribe.yaml
│   ├── vaults/
│   │   ├── leader/CONTEXT.md
│   │   ├── architect/CONTEXT.md
│   │   ├── move-dev/CONTEXT.md
│   │   ├── security-reviewer/CONTEXT.md
│   │   └── scribe/CONTEXT.md
│   └── teams/
│       └── default.yaml
├── sources/                      # Move source code (target_dir)
└── tests/
```

---

## Start a Conversation

```bash
quorum converse \
  "Build a shared-object escrow module that lets two parties swap NFTs atomically"
```

What the daemon prints:

```
[14:00:00] Quorum daemon starting...
[14:00:00] Loaded 5 agents: leader, architect, move-dev, security-reviewer, scribe
[14:00:00] SQLite database: .quorum/quorum.db
[14:00:00] Conversation 1 created — goal: "Build a shared-object escrow..."
[14:00:00] Dispatching to leader...
```

The daemon creates a conversation, sets it to `active`, and hands the goal to the leader.

---

## The Team Mode Loop

One ball, always moving. Here is what a full cycle looks like.

### Step 1: Leader Receives Goal

The leader reads the goal and decides who should work on it first. It responds with a HANDOFF block:

```
I've received the goal. This needs architectural planning before implementation.
The architect should design the module structure, then the move developer
can implement it.

```HANDOFF
to: architect
prompt: Design a shared-object escrow module for atomic NFT swaps on Sui.
Define the module structure, entry functions, and shared object lifecycle.
Consider: (1) how two parties discover and accept a swap, (2) how to handle
partial fulfillment and cancellation, (3) object ownership vs shared objects.
```
```

### Step 2: Thinker Plans

The architect reads the prompt, plans the design, records a knowledge entry, and hands off to the doer:

```
## Escrow Module Design

### Module: `escrow`
- `create_escrow<T1, T2>(initiator_nft: T1, ctx)` — locks initiator's NFT, returns shared Escrow object
- `accept_escrow<T1, T2>(escrow: &mut Escrow, responder_nft: T2, ctx)` — locks responder's NFT, executes swap
- `cancel_escrow<T1, T2>(escrow: Escrow, ctx)` — only initiator can cancel before acceptance

### Shared Object Lifecycle
Escrow is a shared object created on `create_escrow`, consumed on `accept_escrow` or `cancel_escrow`.

```KNOWLEDGE
topic: escrow-architecture
content: Escrow uses shared object pattern. Two entry points: create (locks initiator NFT,
returns shared Escrow) and accept (locks responder NFT, executes atomic swap). Cancel only
available to initiator before acceptance. No partial fulfillment in v1.
```

```HANDOFF
to: move-dev
prompt: Implement the escrow module per the design above. Create sources/escrow.move
with create_escrow, accept_escrow, and cancel_escrow functions. Write tests in
tests/escrow_tests.move covering: successful swap, cancellation, and unauthorized
cancel attempt (should abort). Compile and run tests.
```
```

### Step 3: Doer Implements

The move developer has full tool access (`agent_class: executor`). It writes code in the `target_dir`, compiles, runs tests:

```
I'll implement the escrow module now.

[Creates sources/escrow.move — 85 lines]
[Creates tests/escrow_tests.move — 60 lines]
[Runs: sui move build — success]
[Runs: sui move test — 3/3 tests passed]

All tests pass. The module implements create_escrow, accept_escrow, and
cancel_escrow with proper access control.

```KNOWLEDGE
topic: escrow-implementation
content: Escrow module implemented in sources/escrow.move. 85 lines, 3 entry functions.
All 3 tests pass: successful swap, initiator cancellation, unauthorized cancel abort.
Uses dynamic_object_field to store NFTs inside the shared Escrow object.
```

```HANDOFF
to: security-reviewer
prompt: Review sources/escrow.move for security vulnerabilities. Check for:
(1) reentrancy via shared object access, (2) unauthorized cancellation,
(3) type confusion in generic parameters, (4) object lifecycle correctness.
```
```

### Step 4: Reviewer Validates

The security reviewer reads the code (read-only tools) and either approves or requests changes:

```
## Security Review: escrow.move

### Findings
1. **Access control** — cancel_escrow correctly checks tx_context::sender() == escrow.initiator. PASS.
2. **Object lifecycle** — Escrow consumed on accept and cancel. No dangling shared objects. PASS.
3. **Type safety** — Generic parameters T1, T2 bound by store+key. No type confusion possible. PASS.
4. **Minor issue** — accept_escrow does not verify responder != initiator. Self-swap wastes gas but
   is not a vulnerability. Low priority.

No blocking issues found. Code is safe for deployment.

```KNOWLEDGE
topic: escrow-security-review
content: Security review passed. No vulnerabilities found. Minor note: self-swap not prevented
but is harmless (gas waste only). Access control, object lifecycle, and type safety all verified.
```

```HANDOFF
to: leader
prompt: Security review complete — no blocking issues. The escrow module is ready.
```
```

### Step 5: Leader Decides

The leader has three options:

1. **Done** — goal achieved, trigger scribe, end conversation
2. **Another pass** — hand off to an agent for more work
3. **Ask human** — needs clarification or approval

```
The escrow module is complete. Architecture designed, code implemented, tests passing,
security review clean. Handing off to scribe to record the work.

```HANDOFF
to: scribe
prompt: Record the escrow module work. Summarize the architecture, implementation
decisions, and security review findings from the knowledge ledger.
```
```

After the scribe finishes, the leader marks the conversation done:

```
```HANDOFF
to: done
prompt: Goal complete — escrow module built, tested, and documented.
```
```

The daemon sets the conversation state to `done`.

### Routing Rules

When the daemon receives a HANDOFF block, it resolves the next agent:

1. **HANDOFF `to:` field** — explicit target, always wins
2. **`default_path`** — if no HANDOFF block, follow the configured path
3. **Leader fallback** — if neither, route back to leader

The `to:` field accepts an agent ID (`move-dev`), `human` (pause for user input), or `done` (end conversation).

---

## Human Interaction

When the leader needs human input, it hands off to `human`:

```
I need clarification before proceeding. Should the escrow support
multi-asset swaps (bundle of NFTs) or single-asset only?

```HANDOFF
to: human
prompt: Should the escrow support multi-asset swaps (bundle of NFTs on each side)
or single-asset swaps only? This affects the module design significantly.
```
```

The daemon sets conversation state to `waiting_for_human` and prints:

```
[14:05:30] Conversation 1 waiting for human input
[14:05:30] Question: Should the escrow support multi-asset swaps...
```

### Responding via CLI

```bash
quorum respond \
  --conversation 1 "Single-asset only for v1. We can add bundles later."
```

The daemon resumes the conversation, delivering the human's response to the leader.

### Responding via Web Dashboard

The dashboard shows a conversation card with the pending question and a text input. Type your response and click Send. The daemon picks it up via SSE.

### Other Conversation Commands

```bash
# Check conversation status
quorum status

# Resume a paused conversation (e.g., after budget pause)
quorum resume --conversation 1

# Close a conversation manually
quorum close --conversation 1
```

---

## Knowledge Accumulation

Every agent can emit KNOWLEDGE blocks during its turn. These are append-only entries in the knowledge ledger — a per-conversation log stored in SQLite.

### KNOWLEDGE Block Format

```
```KNOWLEDGE
topic: escrow-architecture
content: Escrow uses shared object pattern. Create locks initiator NFT,
accept executes atomic swap. Cancel only available to initiator.
```
```

An agent can emit multiple KNOWLEDGE blocks in a single turn. The daemon parses them and appends each to the ledger with metadata (agent ID, turn number, timestamp).

### The Ledger

The knowledge ledger is a table in `quorum.db`:

| turn | agent | topic | content |
|------|-------|-------|---------|
| 2 | architect | escrow-architecture | Escrow uses shared object pattern... |
| 3 | move-dev | escrow-implementation | 85 lines, 3 entry functions, all tests pass... |
| 4 | security-reviewer | escrow-security-review | No vulnerabilities found... |

### Scribe Consumption

At the end of a conversation cycle, the leader hands off to a scribe. The scribe receives the full knowledge ledger and produces structured notes — typically Obsidian markdown files written to a configured output directory.

The scribe is a regular agent (`agent_class: analyst`, read-only tools for the project, write access to its notes directory). It reads the ledger, synthesizes findings, and writes notes. The librarian archetype does the same but targets human-facing documentation (READMEs, API docs, changelogs).

---

## Web Dashboard

Quorum ships with a web dashboard (Bun + Hono API server, React frontend).

```bash
cd dashboard && bun install && bun run dev
# → http://localhost:7470
```

The dashboard shows:

- **Conversation cards** — active, waiting, done. Each card shows the goal, current agent, turn count, and cost so far.
- **Real-time updates** — SSE stream from the API server. Agent output appears as it happens.
- **Human interaction** — when a conversation is `waiting_for_human`, the card shows the question and a response input.
- **Task timeline** — every agent invocation with token counts, cost, and duration.
- **Respond controls** — when a conversation is `waiting_for_human`, a text input lets you reply to the leader agent.

---

## Common Scenarios

### "I want to add another doer agent"

```bash
# Generate the agent scaffold (auto-detects role skill, uses template)
quorum agent create --role doer --name ts-dev --target-dir . --no-ai

# Optionally edit the generated CONTEXT.md
vim .quorum/vaults/ts-dev/CONTEXT.md

# Agent is auto-discovered from .quorum/agents/ — no config edits needed
```

Restart the daemon to pick up the new agent.

### "I want to use Quorum for a completely different domain"

Create a new project config. Different agents, different CONTEXT.md files, same daemon:

```bash
# Infrastructure monitoring team
quorum converse \
  "Audit our fullnode configuration and identify performance bottlenecks"
```

The daemon, conversation loop, HANDOFF/KNOWLEDGE parsing, and knowledge ledger work identically. Only the agents change. See `docs/domain-templates.md` for pre-built team compositions.

### "Conversation paused because of budget"

The daemon pauses dispatch when the hourly or daily budget limit is reached:

```
[15:30:00] Budget limit reached (hourly: $2.00/$2.00)
[15:30:00] Conversation 1 paused — will resume when budget resets
```

Resume manually after the budget window resets:

```bash
quorum resume --conversation 1
```

Or wait — the daemon auto-resumes when the hourly window rolls over.

### "An agent is going in circles"

The `max_turns` setting in the project config caps total turns per conversation. When reached, the daemon pauses and escalates to the human:

```
[16:00:00] Conversation 1 reached max_turns (20)
[16:00:00] Conversation 1 paused — max turns reached, awaiting human decision
```

Review the conversation history, then either resume with guidance or close it.

---

## CLI Quick Reference

```bash
# Start a conversation
quorum converse "goal text"

# Check conversation status
quorum status

# Respond to leader (when waiting_for_human)
quorum respond --conversation 1 "your response"

# Resume a paused conversation
quorum resume --conversation 1

# Close a conversation
quorum close --conversation 1
```
