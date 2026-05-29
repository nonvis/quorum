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

## Items scored lighter / N/A here

- **Change-impact reasoning** is exercised by the sibling `architect-impact`
  task, not this one; if the architect volunteers blast-radius reasoning here,
  score it, otherwise treat those items as not-the-focus.
- **Boundary/coupling + invariants** — the clean layering (api→service→repo→db)
  is the obvious boundary call; noting the strict chain + that `db`/`models` are
  leaves earns it. Don't over-penalize a thin treatment on this easy fixture.
