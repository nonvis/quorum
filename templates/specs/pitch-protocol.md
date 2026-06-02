# Quorum pitch-curation protocol spec

Version: **v0.4** (Phase 11 — Librarian as Curator)

Status: authoritative contract for the librarian curation cycle. The C++
daemon and the librarian SKILL.md both implement this document. Mirrors the
shape of `handoff-protocol.md` (scribe write-discipline).

## Why this spec exists

The librarian is being redefined from a one-shot, per-conversation external-docs
writer into a **periodic curator**: it reads accumulated scribe output and
distills it into a structured *aspirational layer* (Pitch / Decision Log /
Roadmap) under `.quorum/librarian/`. The scribe then consults the Pitch as
source-of-truth when deciding what to keep / discard / update / restructure in
its own vault — closing a feedback loop.

The librarian runs **analyst-class at runtime** (read-only; the daemon clamps
`Write/Edit` for every non-`doer` role — see `invoker.h::build_tool_flags`).
So, exactly like the scribe's `LEARNINGS_UPDATE` (handoff-protocol v0.2), the
librarian **emits structured blocks** and the **daemon performs all writes**
via `apply_*` primitives, behind an operator-approval diff gate. The librarian
never writes files itself; do not grant it `Edit`/`Write`.

## File locations

All curation outputs live under **`<project_root>/.quorum/librarian/`**, keeping
the whole curated layer self-contained inside `.quorum/` (mirroring the knower
namespacing, e.g. `.quorum/historian/`). They are human-facing project docs, NOT
agent-loop audit files.

```
<project_root>/
└── .quorum/
    ├── librarian/                   # the curated aspirational layer (v0.2)
    │   ├── Pitch/
    │   │   ├── 00 - Introduction.md # aspirational state — what we're building
    │   │   └── 01 - Anti-goals.md   # what we explicitly will NOT do
    │   ├── 00 - Decision Log.md     # append-only, one entry per scribe decision
    │   └── 01 - Roadmap.md          # phase / open-item tracker
    ├── learnings.md                 # journal input — Decision Log lane (handoff-protocol v0.2)
    ├── knowledge/                   # project-promoted refs (ref-*) — Pitch/Roadmap source (v0.4)
    └── vaults/                      # per-agent vaults
        ├── scribe/knowledge/        # scribe journal notes (secondary context only)
        └── <knower>/knowledge/      # AUTHORITATIVE refs (ref-*) — Pitch/Roadmap source (v0.4)
                                     #   knowers = cartographer / architect / historian / recap
```

The block `file:` fields stay **relative** (`Pitch/00 - Introduction.md`,
`00 - Decision Log.md`, …) — they resolve under `.quorum/librarian/`. Only the
on-disk base directory moved in v0.2; the wire format is unchanged (see
Changelog).

The four output files are the **only** write surface for curation. The librarian
never writes external docs (README/CHANGELOG — that was the *old* librarian role,
retired in Phase 11).

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

Every proposal cites the source it derives from, via the block `source:` field.
As of v0.4 the source depends on the lane: **Pitch / Roadmap** `CURATION_UPDATE`
blocks cite the **knower ref note** (e.g. `vault: architect ref-design.md`);
**Decision Log** `DECISION_LOG_APPEND` blocks cite the `learnings.md` entry
(`learnings.md {utc}`). The librarian distills existing recorded content; it
does not author new claims absent from those sources.

### Rule 8: Operator-owned section lock (hard, apply-time)

A curated section whose **current body** contains the literal marker
`<!-- operator-owned -->` is **never overwritten** by curation. This is the
operator's hard lock: drop the marker anywhere inside a section (e.g. under
`## Current direction`) and `CURATION_UPDATE` will leave that section untouched.

The lock is enforced as a HARD, apply-time check in the write primitive
(`apply_curation_update`), so it protects **both** the CLI (`quorum librarian
curate`) and the daemon (which applies `CURATION_UPDATE` blocks) — there is no way
to route around it. A `CURATION_UPDATE` targeting an operator-owned section is a
**no-op**: the apply returns `ok=true, skipped=true` (deliberately not written),
and the CLI reports it as `skipped (operator-owned)` rather than `applied`.

Scope:
- **Section-scoped.** The lock applies only to the section whose body holds the
  marker; sibling sections in the same file remain curatable.
- **`CURATION_UPDATE` only.** `DECISION_LOG_APPEND` is append-only and never
  overwrites prior content, so it needs no guard and is unaffected.
- **No effect on bootstrap.** A freshly bootstrapped file's skeleton bodies never
  contain the marker, so first creation is never skipped.

The librarian (analyst-class) should not propose changes to a section it sees
carrying the marker; even if it does, the daemon's apply skips the block.

## Input → output source mapping

As of **v0.4**, curated knowledge has **one authoritative home** and the
pipeline is linearized so the librarian layer no longer parallel-derives the
same material the knowers already distilled:

```
scribe journal  →  knower vaults (authoritative, by lens)  →  librarian projection (human view)
```

The **Pitch** and **Roadmap** lanes derive from the **KNOWER VAULTS**
(`.quorum/vaults/<knower>/knowledge/ref-*.md`, knowers =
cartographer / architect / historian / recap, plus project-promoted refs under
`.quorum/knowledge/ref-*.md`) — the authoritative distilled knowledge, by lens.
The **Decision Log**, being chronological, still draws from the scribe journal
(`learnings.md` `decisions`). Curation routes deterministically:

| Source | Curation output | Block | Semantics |
|--------|-----------------|-------|-----------|
| knower vaults (`ref-*`) | `Pitch/00 - Introduction.md` | `CURATION_UPDATE` | refine "What we're building" / "Why it matters" / "Current direction" |
| knower vaults (`ref-*`) | `01 - Roadmap.md` | `CURATION_UPDATE` | "Open items" section |
| knower vaults (`ref-*`) | `Pitch/01 - Anti-goals.md` | `CURATION_UPDATE` | explicit non-goals named in the refs; operator confirms (Q6) |
| `learnings.md` `decisions` | `00 - Decision Log.md` | `DECISION_LOG_APPEND` | one append per decision, chronological (Q7) |

Source discovery for the Pitch/Roadmap lanes: walk `.quorum/vaults/*`, select
the knower roles above; if none match, fall back to **all vaults except
`scribe`**. Only `ref-*.md` notes are read (the durable, retrieval-shaped
knowledge); `rule-*.md` (operating rules) are excluded from the Pitch/Roadmap
source. Each ref's frontmatter `summary:` line (write-for-retrieval, Decision
#44) is preferred as the digest excerpt when present.

The librarian reasons over content *within* each lane (e.g. dedup, summarize,
phrase as an anti-goal), but does not cross lanes — in particular it does NOT
re-derive Pitch/Roadmap content from `learnings.md`. The scribe journal feeds
the Decision Log only. Each Pitch/Roadmap block cites the ref note it derives
from in `source:` (e.g. `vault: architect ref-design.md`); each Decision Log
append cites `learnings.md {utc}`.

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
- `file:` (required) — relative identifier, resolved under
  `<project_root>/.quorum/librarian/`. MUST be one of the four canonical output
  files. Any other path → block dropped with stderr diagnostic.
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
2. The **KNOWER-VAULT digest** — the AUTHORITATIVE distilled knowledge for the
   Pitch + Roadmap lanes: bounded `ref-*.md` filenames + a `summary:`-preferred
   head excerpt across `.quorum/vaults/<knower>/knowledge/` (knowers =
   cartographer / architect / historian / recap; fallback all-but-`scribe`)
   plus `.quorum/knowledge/ref-*.md`. Then `.quorum/learnings.md` as the source
   for the **Decision Log** lane. The scribe-vault digest is included as
   *secondary context only* (to help phrase Decision Log entries), not as a
   Pitch/Roadmap source.
3. Instructions: *"You are the curator. PROJECT the project's authoritative
   knowledge into the aspirational layer. Source of truth (linearized): scribe
   journal → knower vaults (authoritative, by lens) → librarian projection.
   Derive Pitch (What we're building / Why it matters / Current direction) and
   Roadmap (Open items) ONLY from the knower-vault ref notes; derive the
   chronological Decision Log from `learnings.md` `decisions`. Propose
   section-scoped `CURATION_UPDATE` blocks and `DECISION_LOG_APPEND` blocks per
   pitch-protocol v0.4. Route per the source-routing table; do NOT cross lanes
   (in particular do NOT seed Pitch/Roadmap from `learnings.md`). Do NOT invent
   — cite the source (knower ref note for Pitch/Roadmap, `learnings.md {utc}`
   for the Decision Log) in each block's `source:`. Propose only changes not
   already reflected in the current files; if the knower vaults are unchanged
   since the last run, emit no Pitch/Roadmap proposals. Emit no file writes;
   the daemon applies your blocks behind an operator gate."*

## Worked example

Input — three `learnings.md` entries:

```
## Learnings, 2026-05-27T10:00:00Z
### What worked
- Section-scoped diff gate kept operator edits intact across 3 re-runs
### Decisions
- Curation outputs live under .quorum/librarian/ (self-contained curated layer)
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
  Curation outputs live under .quorum/librarian/ (self-contained curated layer).
rationale: |
  Keeping the curated layer inside .quorum/ makes the whole Quorum surface
  self-contained, mirroring the knower dirs (e.g. .quorum/historian/).
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

`ensure_curation_skeleton` only creates *missing* files (under
`.quorum/librarian/` as of v0.2), so existing operator docs are adopted as-is. If
their section headings differ from the canonical schema, `CURATION_UPDATE`
targeting a non-present section is dropped (operator sees the diagnostic and can
rename or add the canonical heading). No automatic migration of legacy heading
names.

**v0.1 → v0.2 location move:** any pre-existing **hand-authored** root-level
`Pitch/`, `00 - Decision Log.md`, or `01 - Roadmap.md` from a v0.1 project are
**unaffected** — curate now reads and writes exclusively under
`.quorum/librarian/`. There is no automatic relocation; the operator may move or
copy a legacy root-level curated layer into `.quorum/librarian/` if they want
curate to continue maintaining it. The block wire format (`file:` identifiers) is
unchanged across the move.

## Changelog

- **v0.4** (Phase 11, 2026-06-02) — **linearized the curation source of truth**:
  `scribe journal → knower vaults (authoritative, by lens) → librarian
  projection`. The **Pitch** (`Pitch/00 - Introduction.md`) and **Roadmap**
  (`01 - Roadmap.md`) lanes now derive from the **KNOWER VAULTS**
  (`.quorum/vaults/<knower>/knowledge/ref-*.md`, knowers = cartographer /
  architect / historian / recap; fallback all-but-`scribe`; plus
  `.quorum/knowledge/ref-*.md` project-promoted refs) — the authoritative
  distilled knowledge — NOT primarily from `learnings.md`. The **Decision Log**
  (`00 - Decision Log.md`), being chronological, still draws from the scribe
  journal (`learnings.md` `decisions`). The prompt now presents a bounded
  knower-vault digest (`ref-*` only — `rule-*` excluded; `summary:`-preferred
  excerpt; same `max_recent`/`head_chars` caps as the scribe digest) as the
  authoritative Pitch/Roadmap source; the scribe-vault digest is retained as
  *secondary context* for Decision Log phrasing only. Rationale: the librarian
  layer and the knower vaults were PARALLEL derivations of the same material and
  could drift; linearizing gives curated knowledge one authoritative home.
  **Wire format, the four output files, the curatable-section set, the `apply_*`
  primitives, and the operator-gate flow are all UNCHANGED** — only input
  sourcing + prompt instructions changed.
- **v0.3** (Phase 11, 2026-05-30) — added the **operator-owned section lock**
  (Rule 8). A curated section whose current body contains the literal marker
  `<!-- operator-owned -->` is never overwritten by curation. The guard is a HARD,
  apply-time check in the write primitive (`apply_curation_update`), so it
  protects both the CLI (`quorum librarian curate`) and the daemon
  (`CURATION_UPDATE` apply) paths. An operator-owned `CURATION_UPDATE` is a no-op:
  apply returns `ok=true, skipped=true`; the CLI reports `skipped
  (operator-owned)`. `DECISION_LOG_APPEND` (append-only) is unaffected; bootstrap
  is unaffected (skeleton bodies carry no marker). Wire format unchanged.
- **v0.2** (Phase 11, 2026-05-30) — moved the curated layer's on-disk location
  from the **project root** to **`<project_root>/.quorum/librarian/`**.
  Rationale: keep the whole Quorum surface self-contained inside `.quorum/`,
  mirroring the knower namespacing (e.g. `.quorum/historian/`). The block **wire
  format is unchanged** — `CURATION_UPDATE` / `DECISION_LOG_APPEND` `file:`
  fields stay relative (`Pitch/00 - Introduction.md`, `00 - Decision Log.md`, …)
  and now resolve under `.quorum/librarian/`. Pre-existing hand-authored
  root-level curated docs are unaffected (no auto-migration; see Migration
  policy).
- **v0.1** (Phase 11, 2026-05-29) — initial spec. Two block types
  (`CURATION_UPDATE`, `DECISION_LOG_APPEND`), four output files, field-mapping
  table, daemon-applies write mechanism, operator-gated diff, bootstrap +
  idempotency rules.
