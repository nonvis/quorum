# Rubrics

Rubrics define what "good" looks like for a given role-specialty. The `evaluator`
agent (Phase 8 Track 1) loads the rubric matching the work being scored, walks
its checklist, and emits per-item pass/fail along with a normalized score.

Format owners: see `quorum-core/src/agent/rubric.h` for the parser, and
Phase 8 Track 4 for the canonical move-dev rubric (forthcoming).

## Layout

```
templates/rubrics/<role-specialty>/rubric.md   # shipped with the daemon
.quorum/rubrics/<role-specialty>/rubric.md     # per-project override (wins)
```

`<role-specialty>` matches the agent role name used elsewhere in the daemon
(e.g. `move-dev`, `ts-dev`, `cpp-dev`). The `evaluator` resolves a rubric by
checking the project override first and falling back to the template that
shipped with the daemon. If neither exists, the evaluator emits no score (the
agent under review is not gated by rubric availability).

## Format

```markdown
---
name: move-dev
version: v1
---

# Rubric: move-dev (v1)

## Compilation & Tests (weight 30)
- [ ] (5) Code compiles cleanly
- [ ] (5) All package tests pass
- [ ] (3) No new warnings

## Move 2024 idioms (weight 25)
- [ ] (4) Uses `public(package)` / `entry` modifiers correctly
- [ ] (3) Receivers named `self` (not `s`, `r`, etc.)
```

Format rules:
- YAML frontmatter with `name` and `version` is **required**. A rubric without
  frontmatter is rejected (parser returns `nullopt`).
- The H1 line is descriptive only — the parser ignores it. The title in the
  frontmatter `name` is the source of truth.
- H2 sections are categories. The optional `(weight N)` annotation on the H2
  line is currently retained for documentation; per-category weights default
  to 10 if absent. Per-item weights drive scoring.
- Items: `- [ ] (N) <description>` where `N` is a positive integer per-item
  weight. The checkbox state in the markdown is **ignored** at parse time —
  the file defines the rubric, not the score. The evaluator marks pass/fail
  per item in its EVALUATION block.
- Items without a `(N)` weight are skipped silently.
- Items with weight `≤ 0` are skipped with a stderr warning.
- Duplicate items (same auto-derived ID) keep the first occurrence and warn
  on subsequent duplicates.

## Item IDs

The parser auto-derives an ID per item by slugifying:

```
<category-slug>.<description-slug>
```

Slugify lowercases, replaces non-alphanumeric runs with hyphens, and trims
leading/trailing hyphens. Example: `Move 2024 idioms` + `Uses public(package) /
entry modifiers correctly` becomes `move-2024-idioms.uses-public-package-entry-modifiers-correctly`.

The IDs are stable across runs as long as the descriptions don't change, so
the evaluator's per-item pass/fail map round-trips cleanly through the
EVALUATION block (Phase 8 Track 3).

## Versioning

Rubrics evolve. The frontmatter `version` field travels with each scored item:
the database row stores `rubric_version` so a six-month-old score can be read
back and interpreted against the rubric that was current when the score was
written. Bump `version` whenever you add, remove, or rebalance items.

## Authoring a new rubric

1. Create `templates/rubrics/<role-specialty>/rubric.md` with frontmatter and
   at least one H2 + one item.
2. Run the parser tests to confirm the rubric loads cleanly:
   `cd build && ctest -R test_rubric_parser`
3. Land the rubric in the same commit as any evaluator-side changes that
   depend on it.

Per-project overrides live at `.quorum/rubrics/<role-specialty>/rubric.md` and
take precedence over the shipped template. Use this when a project has unusual
constraints (e.g., a legacy codebase that intentionally violates a default
idiom).
