---
name: quorum-roles
description: >
  Quorum multi-agent role patterns. Automatically loaded when working
  in a Quorum-managed project. Provides behavioral patterns for leader,
  thinker, doer, scribe, reviewer, librarian, and evaluator agents. Use
  when setting up Quorum agents or debugging agent behavior.
user-invocable: false
---
# Quorum Role Skills

Sub-skills for each Quorum agent role. Each is self-contained.

| Role | Path | When to load |
|------|------|-------------|
| leader | leader/SKILL.md | Agent routes work between team members |
| thinker | thinker/SKILL.md | Agent plans implementation |
| doer | doer/SKILL.md | Agent writes code |
| scribe | scribe/SKILL.md | Agent records outcomes |
| reviewer | reviewer/SKILL.md | Agent validates results |
| librarian | librarian/SKILL.md | Agent writes external docs |
| evaluator | evaluator/SKILL.md | Agent scores work against rubrics (specialty quality, not correctness) |

Load the sub-skill matching your agent's role.
