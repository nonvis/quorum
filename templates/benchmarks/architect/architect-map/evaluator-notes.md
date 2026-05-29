# architect-map — evaluator notes

The fixture code (copied from `expected/`) is the ground truth. Verify the
architect's map against the actual imports + call sites with Read / Grep. No
rigid output diff — score the map by checking each claimed edge exists and that
no real edge is missing.

## Ground-truth component inventory

Five top-level components (one module each):

| Component | Path | Responsibility |
|-----------|------|----------------|
| api | `api.py` | HTTP entry layer; delegates to the service. |
| service | `service.py` | Business logic; validates + builds Order, hands to repository. |
| repository | `repository.py` | Persistence; maps Order to rows, talks to db. |
| db | `db.py` | Low-level connection + in-memory store; the write sink. |
| models | `models.py` | Shared dataclasses (`Order`, `OrderRequest`); leaf module. |

A map that invents a 6th component, or that lists functions/classes as
separate "components", or that merges layers, fails Component inventory.

## Ground-truth edge list (these are the REAL edges — nothing more)

| From | To | Kind | Evidence (file : symbol) |
|------|----|------|--------------------------|
| api | service | calls/depends | `api.py`: `from service import OrderService`; `service.place_order(req)` |
| api | models | depends | `api.py`: `from models import OrderRequest` |
| service | repository | calls/depends | `service.py`: `from repository import OrderRepository`; `self.repo.save(order)` |
| service | models | depends | `service.py`: `from models import Order, OrderRequest` |
| repository | db | calls/depends | `repository.py`: `from db import get_connection`; `get_connection()` / `conn.insert(...)` |
| repository | models | depends | `repository.py`: `from models import Order` |

That is SIX edges. Key checks:
- `db` depends on NOTHING in the project — an edge OUT of `db` to any project
  module is HALLUCINATED. `db.py` has no `from <local>` imports.
- `models` depends on NOTHING in the project (only `dataclasses` stdlib) — an
  edge out of `models` is HALLUCINATED.
- There is NO direct `api → repository`, `api → db`, or `service → db` edge.
  Claiming any of those is a hallucinated edge (the layers are strictly
  chained). Penalize hard under Interconnection accuracy.
- Missing the `service → repository` or `repository → db` edge breaks the
  primary flow — dock "key edges present".

## Ground-truth primary flow (the write path)

Entry point: `api.handle_create_order(payload)`
→ `service.OrderService.place_order(req)`
→ `repository.OrderRepository.save(order)`
→ `db.Connection.insert("orders", ...)` (terminal sink).

`models` is consumed at every layer but is not a flow hop — it is the data
shape that travels along the path. A correct trace names `api.handle_create_order`
as the entry and `db ... insert` as the sink.

## Scoring emphasis

- **Interconnection accuracy (20)** + **Component inventory (15)** carry the
  task. Every edge must cite real file evidence; zero hallucinated edges
  (especially the layer-skip edges above) for full marks.
- **Map form (10)** — a component table + an edge list, not prose.
- **Primary-flow tracing (10)** — the 4-hop chain above with correct entry +
  sink.

## N/A for this task — exclude from scoring (see evaluator SKILL § Job 3 `"na"`)

This is the **map** task. By the suite's design the two architect tasks split
the rubric: `architect-map` exercises inventory + interconnection + map-form +
primary-flow; the sibling `architect-impact` exercises change-impact. This
task's deliverables (component table, edge list, primary-flow trace) propose no
change to reason about. Therefore:

- **Change-impact reasoning — the three `change-impact.*` items (15 wt)** —
  mark `passed: "na"` (excluded from numerator AND denominator; the score
  renormalizes over the remaining ~85 wt). Scoring them `false` because the
  architect didn't volunteer a blast-radius analysis is a scoring error — the
  task never asked for one. **EXCEPTION:** if the architect *does* volunteer a
  change-impact / blast-radius analysis, score those items `true`/`false`
  normally instead of N/A.

Do NOT N/A anything the task DID ask for. Inventory, interconnection, map-form,
primary-flow, and clarity items are all in-scope — score them `true`/`false`.
A requested deep-dive / diagram isn't part of this task either, but the map's
component-table responsibilities and its renderable edge-list shape ordinarily
satisfy `depth.*` and `clarity.*` on their own — score those on what the map
delivers, don't N/A them.

## Boundary / coupling + invariants (10 wt) — in-scope, but easy fixture

In-scope and scored, but light on this tiny fixture. The clean layering
(api→service→repository→db) is the obvious boundary call. For full marks the map
should ALSO surface the two facts the fixture actually has:

- **Coupling hotspot:** `models` has **fan-in 3** — `api`, `service`, AND
  `repository` all import it (`from models import ...`). It's the one shared
  dependency; naming it as the coupling hotspot (not merely "a leaf") earns
  `boundary.coupling-hotspots`.
- **Cross-boundary invariant:** the order **id**. `service` builds the Order
  with `id=0` (`service.py`), and `db.Connection.insert` assigns the real id
  (`db.py`), which `repository.save` writes back onto the Order and returns up
  the chain. "id is created downstream, not by the service" is the load-bearing
  invariant — naming it earns `boundary.invariants`.

A map that notes only the strict chain + that `db`/`models` are leaves earns
`boundary.layering` but not the other two. Don't invent further invariants —
these are the only ones this fixture has; over-claiming is not rewarded.
