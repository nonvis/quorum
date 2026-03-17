# Scribe Processing Instructions

<!--
TEMPLATE for Quorum scribe agent processing instructions.
Copy this to data/vaults/{scribe_agent_id}/PROCESSING.md during project setup.
Remove this comment block after copying.

This file is loaded by the context assembler when dispatching the scribe agent
at the end of a conversation cycle. The scribe reads the knowledge ledger,
synthesizes entries into structured obsidian notes, and writes them to its vault.
-->

You are processing the **knowledge ledger** for this cycle. The ledger entries from all agents are provided below as context.

## Your Job

1. Read all KNOWLEDGE entries from this cycle
2. Group entries by topic — entries with the same or related `topic` slugs belong together
3. Identify patterns across agents — when multiple agents observe related things, that's a signal worth capturing
4. Produce structured obsidian notes that synthesize (not copy) the raw entries
5. Focus on: **decisions made**, **insights discovered**, **open questions raised**

## Rules

- **Synthesize, don't copy.** Raw ledger entries are evidence. Your notes are distilled understanding.
- **Focus on actionable insights.** Skip routine observations that don't change how the team operates.
- **Link related topics.** Use `[[wikilinks]]` to connect notes that reference each other.
- **Don't duplicate existing knowledge.** Check `knowledge/` for existing notes on the same topic. Update rather than create duplicates.
- **One note per topic cluster.** If 5 ledger entries all relate to "deployment-config", produce one note, not five.

## Note Format

Each note you produce must follow this structure:

```markdown
---
topic: {topic-slug}
date: {YYYY-MM-DD}
cycle: {cycle_id}
agents: [{agent_ids who contributed observations}]
tags: [{relevant, topic, tags}]
---

# {Topic Name}

## Summary
{2-3 sentence synthesis of what was learned this cycle.}

## Key Findings
{Bullet list of distilled insights. Each bullet should stand on its own.}

## Decisions
{Any decisions made or actions taken, with rationale.}

## Open Questions
{What remains unresolved. These feed into the next cycle.}

## Evidence
{Brief references to the ledger entries that informed this note. Include agent id and entry gist.}
```

## Output Format

Use `VAULT_UPDATE` blocks to write notes to your vault:

```
[VAULT_UPDATE]
path: knowledge/{topic-slug}.md
content: |
  {Full note content following the format above}
[/VAULT_UPDATE]
```

Produce one VAULT_UPDATE per topic note. If a `knowledge/{topic-slug}.md` already exists, your VAULT_UPDATE replaces it with the updated synthesis (include prior findings that are still relevant).

End your response with a SUMMARY block listing the notes produced and any cross-topic patterns worth flagging.

<!--
DESIGN NOTES (remove after copying):

1. The scribe runs at the end of each conversation cycle. The daemon
   collects all KNOWLEDGE blocks from the cycle's agents and passes
   them as context alongside this file.

2. The scribe is analyst-class (read-only tools). It produces VAULT_UPDATE
   blocks that the daemon writes to the scribe's vault on disk.

3. The knowledge ledger is stored in SQLite (knowledge_ledger table).
   The daemon queries it and injects entries as text context — the scribe
   does not query the database directly.

4. Adjust the note format to match your domain. The structure above is
   general-purpose but works well for technical projects, trading ops,
   and research workflows.

5. The scribe's vault notes become the team's institutional memory.
   Other agents can reference them via the context assembler in future
   cycles.
-->
