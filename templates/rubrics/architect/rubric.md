---
name: architect
version: v1
---

# Rubric: architect (v1)

Source: Quorum Specialties note "02 - Architect Quality Bar" (second-brain
vault). The architect is a thinker (analyst-class, read-only) that understands
a project's STRUCTURE — the components and how they interconnect — and reasons
soundly about it (primary flow, change-impact, coupling, invariants). It does
not write code; it reads, decomposes, maps, and reasons.

The architect's core output is a factual claim about the codebase, and the code
is the ground truth. The evaluator (analyst, Read/Grep) verifies the map
against the real repo: do the named components exist, do the claimed
dependency/data-flow edges match the actual includes/imports/call sites (no
hallucinated edges, no missing key edges), does the traced flow match real
entry points and sinks. That makes the bulk of the bar objective; the judgment
dimensions (change-impact, coupling, invariants) carry concrete sub-criteria.

The architect persists a structured `ref-architecture-map.md` (a component
table + an edge list) and never renders diagrams — it emits a free-form textual
diagram description that claude.ai draws.

The `evaluator` agent (Phase 8 Track 1) reads this file, walks each item, and
emits per-item pass/fail in the EVALUATION block (Phase 8 Track 3). Categories
are documentation; per-item `(W)` weights drive scoring.

## Interconnection accuracy (weight 20)
- [ ] (7) Every claimed dependency / call / data-flow edge cites file evidence (import line, include, call site) and that evidence checks out against the code
- [ ] (7) No hallucinated edges — every edge in the map corresponds to a real relationship in the repo
- [ ] (4) Key edges are present — no major real interconnection between top-level components is missing
- [ ] (2) INFERRED / doc-only / config-derived edges are flagged distinctly from code-verified edges

## Component inventory (weight 15)
- [ ] (6) All top-level components are identified — nothing major missed (verify against the repo's module / directory / build-target structure)
- [ ] (5) Each named component actually exists as a distinct unit in the repo (no fabricated components)
- [ ] (4) Granularity is right — top-level components, not a raw file listing and not so coarse that distinct components are merged

## Map form — structured (weight 10)
- [ ] (5) The map is stored as a component table (name · path · responsibility), not prose
- [ ] (3) An edge list (from → to · kind: depends / calls / data-flow) is present and machine-readable
- [ ] (2) A staleness stamp (`mapped against <commit/date>`) is present

## Per-component depth (weight 10)
- [ ] (5) A requested deep-dive correctly states the component's responsibility and is consistent with its code
- [ ] (3) The component's public interface (exported functions / types / endpoints) is described and matches the code
- [ ] (2) What the component touches (the things it depends on / is depended on by) is stated and consistent with the edge list

## Primary-flow tracing (weight 10)
- [ ] (5) The main use-case path is traced end-to-end across components with the correct ordered hops
- [ ] (3) The correct entry point(s) are identified for the traced flow
- [ ] (2) The correct sink / terminal effect of the flow is identified

## Change-impact reasoning (weight 15)
- [ ] (7) Given a hypothetical change, the affected components (blast radius) are correctly identified
- [ ] (5) The reasoning follows the actual edges — affected components are reachable from the changed one via real dependencies
- [ ] (3) Unaffected components are not over-claimed — the blast radius is scoped, not "everything"

## Boundary / coupling + invariants (weight 10)
- [ ] (4) Layering / boundaries between components are identified with rationale tied to the code
- [ ] (3) Coupling hotspots (components with outsized fan-in / fan-out) are called out
- [ ] (3) Load-bearing invariants (a constraint the system relies on across a boundary) are identified with where they're enforced

## Clarity + diagram renderability (weight 10)
- [ ] (4) The map is navigable — a reader can locate a component and its edges without ambiguity
- [ ] (4) A requested diagram description is complete + unambiguous enough for claude.ai to render faithfully (nodes, edges, grouping, layout intent)
- [ ] (2) The architect emits a textual diagram description and does NOT attempt to render a diagram itself
