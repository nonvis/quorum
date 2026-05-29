# Quorum pitch-curation protocol spec

Version: **v0.1** (Phase 11 — Librarian as Curator)

Status: authoritative contract for the librarian curation cycle. The C++
daemon and the librarian SKILL.md both implement this document. Mirrors the
shape of `handoff-protocol.md` (scribe write-discipline).

## Why this spec exists

The librarian is being redefined from a one-shot, per-conversation external-docs
writer into a **periodic curator**: it reads accumulated scribe output and
distills it into a structured *aspirational layer* (Pitch / Decision Log /
Roadmap) at the project root. The scribe then consults the Pitch as
source-of-truth when deciding what to keep / discard / update / restructure in
its own vault — closing a feedback loop.

The librarian runs **analyst-class at runtime** (read-only; the daemon clamps
`Write/Edit` for every non-`doer` role — see `invoker.h::build_tool_flags`).
So, exactly like the scribe's `LEARNINGS_UPDATE` (handoff-protocol v0.2), the
librarian **emits structured blocks** and the **daemon performs all writes**
via `apply_*` primitives, behind an operator-approval diff gate. The librarian
never writes files itself; do not grant it `Edit`/`Write`.

## File locations

All curation outputs live at the **target project root** (sibling of `.quorum/`),
mirroring the operator's own PARA layout. They are human-facing project docs,
NOT agent-loop audit files.

```
<project_root>/
├── Pitch/
│   ├── 00 - Introduction.md     # aspirational state — what we're building
│   └── 01 - Anti-goals.md       # what we explicitly will NOT do
├── 00 - Decision Log.md         # append-only, one entry per scribe decision
├── 01 - Roadmap.md              # phase / open-item tracker
└── .quorum/
    ├── learnings.md             # scribe input (handoff-protocol v0.2)
    └── vaults/scribe/knowledge/ # scribe input (per-conv notes, rule-*/ref-*)
```

The four output files are the **only** write surface for curation. The librarian
never writes under `.quorum/` (that is the scribe's surface) and never writes
external docs (README/CHANGELOG — that was the *old* librarian role, retired in
Phase 11).

## The rules

### Rule 1: Daemon writes, librarian proposes

The librarian emits `CURATION_UPDATE` and `DECISION_LOG_APPEND` blocks. The
daemon parses them, renders a diff, and writes only on operator approval. The
librarian has no file-write tools. This is the same write-mechanism inversion
as scribe `LEARNINGS_UPDATE`.

### Rule 2: Operator-gated diff

Every proposed write is shown to the operator as a unified diff before it lands.
`quorum librarian curate` is interactive (approve per file); `--dry-run` previews
without writing; `--apply` writes all proposed changes without prompting (for
non-interactive use). Default (no flag) = interactive per-file approve.

### Rule 3: Section-scoped edits preserve operator content

`CURATION_UPDATE` replaces the body of **one named canonical section** in one
file. Content outside the named section — including operator hand-edits in other
sections — is never touched. This is the core trust property: the operator can
edit freely and curation will not clobber it.

### Rule 4: Decision Log is append-only

`DECISION_LOG_APPEND` only appends a new timestamped entry. Prior entries are
byte-preserved, never rewritten or reordered. Mirrors `learnings.md` append-only
discipline (handoff-protocol Rule 2).

### Rule 5: Bootstrap on first curate, never overwrite

`quorum librarian curate` first calls `ensure_curation_skeleton(project_root)`,
which creates any of the four output files that are missing, with canonical
empty-section structure. Existing files are left byte-identical (never
overwritten by bootstrap). Skills cooperate, they do not gate: no "run setup
first" message, no dependency-chain warnings (Suiperpower handoff-spec Rule 4).

### Rule 6: Idempotency

Re-running `curate` with no new scribe input since the last run produces no
diff. The librarian proposes only changes derived from learnings/vault content
not already reflected in the aspirational layer.

### Rule 7: Cite the source, do not invent

Every proposal cites the `learnings.md` entry (or scribe note) it derives from,
via the block `source:` field. The librarian distills existing recorded content;
it does not author new claims the scribe never recorded.

## Input → output field mapping

The scribe's `learnings.md` uses five canonical fields per session entry
(handoff-protocol v0.2). Curation routes them deterministically:

| `learnings.md` field | Curation output | Block | Semantics |
|----------------------|-----------------|-------|-----------|
| `decisions`          | `00 - Decision Log.md` | `DECISION_LOG_APPEND` | one append per decision (Q7) |
| `did_not_work`       | `Pitch/01 - Anti-goals.md` | `CURATION_UPDATE` | proposed anti-goals; operator confirms (Q6) |
| `worked` + `tried`   | `Pitch/00 - Introduction.md` | `CURATION_UPDATE` | refine "What we're building" / "Current direction" |
| `open_questions`     | `01 - Roadmap.md` | `CURATION_UPDATE` | "Open items" section |

The librarian reasons over content *within* each lane (e.g. dedup, summarize,
phrase as an anti-goal), but does not cross lanes.

## Block formats

### CURATION_UPDATE — section-scoped replace

```CURATION_UPDATE
file: Pitch/00 - Introduction.md
section: What we're building
content: |
  - {distilled bullet derived from scribe `worked`/`tried`}
  - {bullet}
source: learnings.md 2026-05-28T14:32:11Z, 2026-05-29T09:10:00Z
```

Fields:
- `file:` (required) — path relative to project root. MUST be one of the four
  canonical output files. Any other path → block dropped with stderr diagnostic.
- `section:` (required) — canonical section heading text, WITHOUT the leading
  `## `. MUST be a canonical section for that file (see schema below). Unknown
  section → block dropped.
- `content:` (required) — `key: |` multi-line YAML form, 2-space indented. The
  replacement body for that section (everything between the `## {section}`
  heading and the next `## ` heading or EOF).
- `source:` (optional) — provenance citation.

Daemon behavior: locate `## {section}` in `{file}`, replace its body with
`content`, preserve the heading and all other sections, atomic writeback.
Missing required field → drop block, stderr diagnostic, no write.

### DECISION_LOG_APPEND — append-only

```DECISION_LOG_APPEND
utc: 2026-05-29T09:10:00Z
decision: |
  Adopt block + daemon-applies for librarian writes.
rationale: |
  Librarian is analyst-class at runtime; SKILL.md "executor" claim is
  aspirational. Daemon-applies keeps the approval gate real.
source: learnings.md 2026-05-29T09:10:00Z
```

Fields:
- `utc:` (required) — UTC ISO-8601 with `Z`. Missing/empty → block dropped
  (mirrors `LEARNINGS_UPDATE` `utc` gate).
- `decision:` (required) — the decision statement (`key: |` form).
- `rationale:` (optional) — the "why".
- `source:` (optional) — provenance citation.

Daemon behavior: append a `### {date-from-utc} — {decision first line}` entry
with a `**Why:**` line and a `**Source:**` line to `00 - Decision Log.md`. Never
rewrites prior entries. Bootstraps the file if missing.

## Canonical schema for output files

### `Pitch/00 - Introduction.md`

```
---
title: {Project} — Pitch
updated: {date}
---

# {Project}

## What we're building

## Why it matters

## Current direction

## What we're NOT doing

See [[01 - Anti-goals]].
```

Curatable sections (valid `CURATION_UPDATE` targets): `What we're building`,
`Why it matters`, `Current direction`.

### `Pitch/01 - Anti-goals.md`

```
---
title: Anti-goals
updated: {date}
---

# Anti-goals

## Anti-goals

- {anti-goal} — {why} ({date})
```

Curatable section: `Anti-goals`.

### `00 - Decision Log.md`

```
---
title: Decision Log
updated: {date}
---

# Decision Log

Append-only. One entry per scribe-flagged decision.

### {date} — {decision title}

**Why:** {rationale}

**Source:** learnings.md {utc}
```

Append target only (via `DECISION_LOG_APPEND`); not a `CURATION_UPDATE` target.

### `01 - Roadmap.md`

```
---
title: Roadmap
updated: {date}
---

# Roadmap

## Open items

- {distilled open question}
```

Curatable section: `Open items`.

## Curation prompt (what the CLI feeds the librarian)

`quorum librarian curate` invokes the librarian agent (analyst-class,
synchronous) with:

1. The current contents of the four output files (so it proposes *deltas*, not
   restatements — Rule 6 idempotency).
2. `.quorum/learnings.md` and a digest of the scribe vault.
3. Instructions: *"You are the curator. Distill the scribe's recorded learnings
   into the aspirational layer. Propose section-scoped `CURATION_UPDATE` blocks
   and `DECISION_LOG_APPEND` blocks per pitch-protocol v0.1. Route fields per the
   mapping table. Do NOT invent — cite the source learnings entry in each block's
   `source:`. Propose only changes not already reflected in the current files.
   Emit no file writes; the daemon applies your blocks behind an operator gate."*

## Worked example

Input — three `learnings.md` entries:

```
## Learnings, 2026-05-27T10:00:00Z
### What worked
- Section-scoped diff gate kept operator edits intact across 3 re-runs
### Decisions
- Curation outputs live at project root, not under .quorum/
## Learnings, 2026-05-28T14:00:00Z
### What did not work
- Granting the librarian executor tools — daemon clamps non-doer roles read-only
### Open questions
- Should auto-after-N-sessions trigger ship in v0.1?
## Learnings, 2026-05-29T09:00:00Z
### Decisions
- Manual CLI trigger only for v0.1
```

Output — librarian emits:

```CURATION_UPDATE
file: Pitch/00 - Introduction.md
section: Current direction
content: |
  - Section-scoped diff gate preserves operator edits across re-runs.
source: learnings.md 2026-05-27T10:00:00Z
```
```CURATION_UPDATE
file: Pitch/01 - Anti-goals.md
section: Anti-goals
content: |
  - Do NOT grant the librarian executor tools — the daemon clamps non-doer
    roles read-only; writes go through daemon-applied blocks. (2026-05-28)
source: learnings.md 2026-05-28T14:00:00Z
```
```CURATION_UPDATE
file: 01 - Roadmap.md
section: Open items
content: |
  - Decide whether auto-after-N-sessions curation trigger ships post-v0.1.
source: learnings.md 2026-05-28T14:00:00Z
```
```DECISION_LOG_APPEND
utc: 2026-05-27T10:00:00Z
decision: |
  Curation outputs live at the project root, not under .quorum/.
rationale: |
  Aspirational layer is human-facing project docs; .quorum/ stays the
  agent-loop audit surface.
source: learnings.md 2026-05-27T10:00:00Z
```
```DECISION_LOG_APPEND
utc: 2026-05-29T09:00:00Z
decision: |
  Manual CLI trigger only for v0.1 (quorum librarian curate).
rationale: |
  Matches the manual-gate posture of Phase 10 hygiene CLIs; auto-trigger is a
  documented v0.2 follow-up.
source: learnings.md 2026-05-29T09:00:00Z
```

The daemon renders one diff per affected file, the operator approves, and the
five proposals land in four files (two appends to the Decision Log).

## Migration policy

v0.1 is the first version. Projects with a hand-authored Pitch/Decision-Log/
Roadmap predating Phase 11: `ensure_curation_skeleton` only creates *missing*
files, so existing operator docs are adopted as-is. If their section headings
differ from the canonical schema, `CURATION_UPDATE` targeting a non-present
section is dropped (operator sees the diagnostic and can rename or add the
canonical heading). No automatic migration of legacy heading names in v0.1.

## Changelog

- **v0.1** (Phase 11, 2026-05-29) — initial spec. Two block types
  (`CURATION_UPDATE`, `DECISION_LOG_APPEND`), four output files, field-mapping
  table, daemon-applies write mechanism, operator-gated diff, bootstrap +
  idempotency rules.
