---
name: architect-impact
description: Reason about the change-impact / blast radius of a change in a small multi-module project.
---

# Architect Change-Impact Benchmark

## Goal

The project in the root is a small multi-module pricing service. First, map its
components + interconnections (component table + edge list with file evidence),
then answer the change-impact question:

> A teammate wants to change the return type of `fees.compute_fee` from a flat
> `int` to a `(int, str)` tuple `(amount, currency)`. **What is the blast
> radius** — which components / call sites are affected and why, and which are
> NOT affected?

## Constraints

- Output is a structured map (component table + edge list), not prose.
- Every interconnection edge must cite real file evidence (import / call site).
  No hallucinated edges.
- The change-impact answer must follow the ACTUAL edges: a component is in the
  blast radius only if it reaches `fees.compute_fee` through a real dependency /
  call. Do not over-claim ("everything is affected") and do not miss a real
  caller.
- Name the unaffected components explicitly and say why they are insulated.
- Include a staleness stamp.

## What to deliver

- Component table + edge list (with evidence).
- Blast-radius analysis: affected components + the specific call sites that
  break, the reasoning along the edges, and the components that are NOT
  affected.

The evaluator scores this against the architect rubric, weighting Change-impact
reasoning. The code is the ground truth — the set of real callers of
`compute_fee` is checkable directly.
