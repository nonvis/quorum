---
name: quorum-roles
description: >
  Quorum multi-agent role patterns. Automatically loaded when working
  in a Quorum-managed project. Provides behavioral patterns for leader,
  thinker, doer, and evaluator agents plus the four knowers. Use
  when setting up Quorum agents or debugging agent behavior.
user-invocable: false
---
# Quorum Role Skills

Sub-skills for each Quorum agent role. Each is self-contained.

## Core roles

| Role | Path | When to load |
|------|------|-------------|
| leader | leader/SKILL.md | Agent routes work between team members |
| thinker | thinker/SKILL.md | Agent plans implementation |
| doer | doer/SKILL.md | Agent writes code |
| evaluator | evaluator/SKILL.md | Agent scores work against rubrics (specialty quality, not correctness) |

The pipeline is leader → thinker → doer → (evaluator). The **doer** (or
the **evaluator**, when one is in the team) is the terminal stage and
hands off to `done`. There is no scribe and no librarian — the daemon
persists every conversation automatically.

## Knowers (the four read-only lenses)

The four knowers are the **sole knowledge accumulators**. Each is a
read-only lens that answers a different question about the codebase and
self-writes its own vault during `quorum knower refresh`:

| Knower | Question |
|--------|----------|
| cartographer | where? (structure / layout) |
| architect | how? (mechanism / design) |
| historian | why? (decisions / rationale) |
| recap | what / when? (state / catch-up) |

`quorum ask` answers from the knower vaults, not from a live scan.

## Brainstorm

Brainstorm mode is **read-only and human-gated**: the leader orchestrates
a read-only discussion, presents the human a per-knower manifest of what
would be written where, and only on approval do the participating knowers
self-write their own vault slices. No agent writes another agent's vault.

Load the sub-skill matching your agent's role.
