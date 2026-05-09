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

**Two modes:** every conversation runs as either **generic** (default — agents mutate the project, doers write real artifacts) or **brainstorm** (every agent is read-only at the tool layer; scribe distributes curated knowledge cross-vault at the end). Sequential dispatch, HANDOFF protocol, and the agent roster are identical in both. Only the write surface differs.

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
model: opus                                               # optional: per-agent model override
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
model: sonnet                                               # optional: cheaper model for planning
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
decisions, and security review findings from the conversation transcript.
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

1. **HANDOFF `to:` field** — explicit target, always wins. If the target is in `default_path`, `path_index` is synced so subsequent no-HANDOFF turns route correctly.
2. **`default_path`** — if no HANDOFF block, follow the configured path (increment `path_index`)
3. **End of path** — if no HANDOFF and `default_path` exhausted, conversation completes
4. **Unknown agent** — fallback to leader

The `to:` field accepts an agent ID (`move-dev`), `human` (pause for user input), or `done` (end conversation).

---

## Brainstorm Mode Flow

Same team, same dispatch, same HANDOFF protocol — but the project never changes. Brainstorm mode is for getting the team smarter, not for shipping artifacts.

### Step 0: User Invokes Brainstorm

```bash
quorum converse --mode brainstorm \
  "How should we partition the escrow module if we add multi-asset swaps later?
   I'm not asking you to build it — I want the team to think out loud."
```

The daemon prints:

```
[14:00:00] Conversation 2 created — mode: brainstorm
[14:00:00] Goal: "How should we partition the escrow module if we add..."
[14:00:00] Dispatching to leader...
```

The conversation row is inserted with `mode='brainstorm'`. `ConversationRecord` carries that field through the whole cycle.

### Step 1: Invoker Clamps Tools to Read-Only

Every agent's `claude -p` invocation in this conversation gets analyst-class tool flags, regardless of role. The invoker reads `conversation.mode` and overrides:

```bash
# What the invoker spawns for the move-dev agent (a doer) in brainstorm mode:
claude -p "prompt" --dangerously-skip-permissions \
  --disallowedTools "Write,Edit,NotebookEdit" --output-format json
```

The doer's `target_dir` is irrelevant — it can't write there anyway. Doers in brainstorm mode are reasoning participants, not executors.

### Step 2: Agents Reason Together

The leader, architect, move-dev, and security-reviewer pass the ball through HANDOFF blocks the same way they do in generic mode. They produce SUMMARY and HANDOFF blocks. They reference each other's work. They debate trade-offs.

What they do **not** do:
- Write code. The doer can't, and won't try to.
- Modify project files of any kind. The tool layer forbids it.
- Write into their own vaults during the cycle. (Mid-cycle VAULT_UPDATE blocks remain scoped to each agent's own vault, just as in generic mode.)

### Step 3: Scribe Distributes Knowledge Cross-Vault

At the end of the cycle, the leader hands off to scribe. The scribe reads the full transcript and produces VAULT_UPDATE blocks whose paths target **other** agents' vaults:

```
```VAULT_UPDATE
path: architect/knowledge/rule-escrow-partition-when-to-split.md
content: |
  When the escrow module's swap surface grows beyond single-asset, prefer a
  separate `escrow_bundle` module over generic-parameter overload. ...
```

```VAULT_UPDATE
path: move-dev/knowledge/ref-multi-asset-swap-patterns.md
content: |
  Reference patterns for multi-asset swaps on Sui: ...
```

```VAULT_UPDATE
path: security-reviewer/knowledge/rule-bundle-swap-attack-surface.md
content: |
  Bundle swaps expand the attack surface in three ways: ...
```
```

These are cross-vault writes — the scribe's own vault directory is `.quorum/vaults/scribe/`, but the paths above resolve to `architect/`, `move-dev/`, and `security-reviewer/`.

### Step 4: VaultManager Validates the Cross-Vault Writes

`VaultManager::apply_all_updates_with_context` is the gate. For each VAULT_UPDATE block:

1. If the path stays inside the writer's own vault → allowed in any mode.
2. If the path targets another agent's vault → **only allowed if** (a) the writer's role is `scribe` *and* (b) the conversation mode is `brainstorm`.

Anything else is rejected. A doer can't sneak a cross-vault write in generic mode. A scribe can't sneak one in generic mode either.

### Step 5: Cycle Ends — Project Unchanged, Vaults Smarter

```
[14:08:42] Conversation 2 done — mode: brainstorm
[14:08:42] Project files modified: 0
[14:08:42] Vault knowledge files written: 3 (across architect, move-dev, security-reviewer)
```

```
my-move-project/
├── sources/                            # ← unchanged
├── tests/                              # ← unchanged
└── .quorum/
    └── vaults/
        ├── architect/knowledge/
        │   └── rule-escrow-partition-when-to-split.md       # ← new
        ├── move-dev/knowledge/
        │   └── ref-multi-asset-swap-patterns.md             # ← new
        └── security-reviewer/knowledge/
            └── rule-bundle-swap-attack-surface.md           # ← new
```

The next time any of these agents runs (in either mode), the context assembler picks up the new knowledge file and loads it into the prompt.

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

At the end of a conversation cycle, the leader hands off to a scribe. The scribe reads the full conversation transcript (via `claude -p` session resume) and produces structured notes — typically Obsidian markdown files written to a configured output directory.

The scribe is a regular agent (`agent_class: analyst`, read-only tools for the project, write access to its notes directory). It synthesizes findings from the transcript and writes notes. The librarian archetype does the same but targets human-facing documentation (READMEs, API docs, changelogs).

Knowledge accumulates over time as scribe-distilled notes load into future agent invocations.

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

The daemon, conversation loop, and HANDOFF parsing work identically. Only the agents change. See `docs/domain-templates.md` for pre-built team compositions.

### "Window budget exhausted"

The daemon pauses dispatch when the window budget is exhausted:

```
[15:30:00] Window budget exhausted ($100.00/$100.00)
[15:30:00] Dispatch paused — will resume when window resets
```

The window budget covers all conversations, not individual ones. When exhausted, no new tasks are dispatched until the window resets (after `window_hours`) or the budget is increased via the web UI.

```bash
# Or resume manually after increasing budget
quorum resume --conversation 1
```

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
# Start a conversation (generic mode — agents mutate the project)
quorum converse "goal text"

# Start a brainstorm conversation (read-only team, cross-vault knowledge)
quorum converse --mode brainstorm "goal text"

# Check conversation status
quorum status

# Respond to leader (when waiting_for_human)
quorum respond --conversation 1 "your response"

# Resume a paused conversation
quorum resume --conversation 1

# Close a conversation
quorum close --conversation 1
```
