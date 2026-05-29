# architect-impact — evaluator notes

The fixture code (from `expected/`) is the ground truth. This benchmark
stresses **Change-impact reasoning** — the architect must follow the real edges,
not guess. Verify every claim with Read / Grep.

## Ground-truth component inventory

| Component | Path | Responsibility |
|-----------|------|----------------|
| fees | `fees.py` | fee computation; `compute_fee(subtotal)` — the unit under change. |
| cart | `cart.py` | line total incl. fee; direct caller of `compute_fee`. |
| checkout | `checkout.py` | finalizes an order via `cart_total`; does NOT call `compute_fee` directly. |
| report | `report.py` | sums fees over a batch; second direct caller of `compute_fee`. |
| catalog | `catalog.py` | product sticker prices; touches no fee logic. |

## Ground-truth edge list (the only real edges)

| From | To | Kind | Evidence |
|------|----|------|----------|
| cart | fees | calls | `cart.py`: `from fees import compute_fee`; `compute_fee(subtotal)` |
| checkout | cart | calls | `checkout.py`: `from cart import cart_total`; `cart_total(subtotal)` |
| report | fees | calls | `report.py`: `from fees import compute_fee`; `compute_fee(s)` |

Three edges. `catalog` has NO edges (in or out, within the project). Any edge
to/from `catalog`, or any edge like `checkout → fees` (it does not import fees),
is HALLUCINATED.

## Ground-truth blast radius for "change `compute_fee` to return `(int, str)`"

DIRECT callers (their call sites break — they consume the return value as an
`int`):
- **cart** — `cart.py`: `fee = compute_fee(subtotal); return subtotal + fee`
  (adds an int; a tuple breaks the arithmetic).
- **report** — `report.py`: `sum(compute_fee(s) for s in subtotals)`
  (sums ints; a tuple breaks `sum`).

TRANSITIVELY affected (no direct call, but depends on a broken component):
- **checkout** — calls `cart.cart_total`; if `cart` changes shape/behavior to
  handle the tuple, checkout's contract may shift. It does NOT call
  `compute_fee` itself, so a correct answer flags it as *transitively /
  indirectly* affected via `cart`, distinct from the direct breakers. (Accept
  either "indirectly affected through cart" or a well-argued "insulated as long
  as cart_total keeps returning an int"; the key is the reasoning follows the
  cart edge, not a hallucinated checkout→fees edge.)

NOT affected (insulated):
- **catalog** — no path to `fees` whatsoever. A correct answer names catalog as
  unaffected and says why (no dependency on fees).
- **fees** itself is the changed unit, not "affected by" the change.

## Scoring emphasis

- **Change-impact reasoning (15)** — full weight. Must (a) identify cart + report
  as the direct breakers, (b) reason about checkout via the cart edge (not a
  fabricated direct edge), and (c) name catalog as NOT affected. Over-claiming
  "everything breaks" (including catalog) fails the "blast radius is scoped"
  item. Missing report (the second direct caller) fails the "affected
  components correctly identified" item.
- **Interconnection accuracy (20)** — the three real edges, evidence-cited, zero
  hallucinated edges (no `checkout → fees`, no `catalog` edges).
- **Component inventory (15)** — all five modules, none invented.

## Items scored lighter here

- Primary-flow tracing, per-component depth, diagram renderability — present but
  secondary; the change-impact reasoning is the discriminating signal.
