---
name: architect-map
description: Map the components and interconnections of a tiny multi-module Python service.
---

# Architect Map Benchmark

## Goal

Map the architecture of the project in the root. It is a small multi-module
Python service. Produce:

1. A **component inventory** — the top-level components (modules), each with
   its path and a one-line responsibility.
2. An **interconnection map** — an edge list (from → to · kind:
   depends / calls / data-flow), one edge per real relationship between
   components, each citing the file evidence. Cite the **exact line the import
   or call sits on**, not the enclosing function/class definition — a `calls`
   edge points at the call-site line, not the `def`/`class` line above it.
3. A **primary-flow trace** — trace the main request path end-to-end across
   components, naming the entry point and the terminal sink.
4. A short **coupling & invariants note** — name any coupling hotspots
   (components with notably high fan-in or fan-out) and any cross-boundary
   invariant the design relies on (a constraint one component depends on another
   to uphold). One or two lines; tie each to the code. (Do not invent
   invariants the code doesn't have.)

## Constraints

- Store the output as a structured map: a component table (name · path ·
  responsibility) + an edge list — NOT a prose essay.
- Every edge MUST cite file evidence (an import or a call site) that exists in
  the code. No hallucinated edges. If you infer an edge from something other
  than code (a comment, a config), flag it as INFERRED distinctly.
- Identify the top-level components — do not list every file as a component,
  and do not merge distinct modules into one.
- Include a staleness stamp (the date / commit you mapped against).
- You may emit a free-form textual diagram description; do NOT attempt to
  render an actual diagram.

## What to deliver

- The component table.
- The edge list with per-edge file evidence (exact import/call-site lines).
- The primary-flow trace (entry point → ... → sink).
- A short coupling-hotspots + cross-boundary-invariants note.

The evaluator scores this against the architect rubric. The code is the ground
truth — every claimed edge is checked against the real imports/call sites; no
hallucinated edges, no missing key edges.
