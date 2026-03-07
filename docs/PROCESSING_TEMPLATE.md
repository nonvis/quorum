# Knowledge Base Processing Instructions

<!--
TEMPLATE for Quorum knowledge base librarian instructions.
Copy this to data/knowledge/PROCESSING.md during quorum init or manual setup.
Remove this comment block after copying.

This file is loaded by the context assembler when dispatching inbox_process
tasks. The librarian agent reads inbox/ notes, synthesizes them into
library/ topic files, and marks inbox notes as processed.
-->

You are processing the knowledge base inbox. Your job:

1. Read each unprocessed note in inbox/
2. For each note:
   a. Identify the topic (e.g., adverse-selection, spread-capture, regime)
   b. Check if library/{topic}/findings.md exists
   c. If yes: merge new observations into the existing findings
   d. If no: create a new topic folder and findings.md
3. Update the inbox note's frontmatter: set processed: true
4. If you discover cross-topic patterns, write them as new OBSERVATION blocks

## Rules

- Library findings.md files are OVERWRITTEN, not appended — always write the complete current understanding
- Keep findings concise — current understanding only, not a changelog
- Move library topics to archive/ if all evidence is >30 days old and superseded
- Never delete inbox notes — only mark as processed

## Library Note Format

Each library topic should follow this structure:

```markdown
---
topic: {topic-name}
last_updated: {YYYY-MM-DD}
observation_count: {N}
---

# {Topic Name} — Current Understanding

## Pattern
{What we know — distilled from all observations.}

## Evidence
{Bullet list of supporting observations with dates.}

## Open Questions
{What we don't know yet.}
```

## Output Format

Use structured blocks in your response text:

- `VAULT_UPDATE` with path `library/{topic}/findings.md` — for new or updated topic files
- `VAULT_UPDATE` with path to inbox note — to set `processed: true` in frontmatter
- `OBSERVATION` — for cross-topic patterns discovered during synthesis

<!--
DESIGN NOTES (remove after copying):

1. This file is loaded once per inbox_process invocation. It does not
   need to be small — it's only loaded for librarian tasks, not every
   agent invocation.

2. The librarian uses an existing agent role (typically bot_analyst or
   market_analyst). It gets this file as additional context on top of
   its own CONTEXT.md.

3. Modify the topic examples and library note format to match your
   domain. The structure above assumes a trading/DeFi context but
   works for any domain.

4. Processing frequency is configured in the daemon scheduler
   (default: daily or every 12h). Adjust based on inbox volume.
-->
