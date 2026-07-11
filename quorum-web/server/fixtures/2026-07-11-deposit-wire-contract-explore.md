# Autopilot checkpoint

Created at: 2026-07-10T22:03:11Z
Updated at: 2026-07-11T03:26:54Z
Flight spec: 0.3
Fixture: true (synthetic demo flight — safe to delete)
Mode: brainstorm
Goal: Overnight explore — unify the deposit wire contract across listener and service

## Major tasks

- [x] Task 1: Inventory every deposit payload shape — catalog all producers/consumers of deposit events across the 7 repos with file evidence
- [>] Task 2: Evaluate unification options — batched HistoryDto[] everywhere vs versioned envelope vs per-topic contracts; score migration cost per repo
- [ ] Task 3: Draft the decision note — write the proposed wire-contract decision with rollout order for the historian's decision log

## Condensed outcomes

### Task 1 — payload shape inventory
- Three disagreeing shapes confirmed: sui-listener emits single `DepositEvent` on `blockchain_listener.crypto_deposit`; EVM/SVM listeners emit batched `HistoryDto[]` on `sweeping-{ENV}-deposits`; the Rust indexer's outbox rows are a fourth, DB-internal shape that the TS publisher already normalizes.
- Verdict: the listener→service hop is DOA only for Sui; EVM/SVM paths agree with the consumer. Full shape × topic × repo matrix written into the flight notes.

### Task 2 — unification options (in flight)
- Option A (batch everywhere) scored so far: 2 repos touched, backwards-compatible for EVM/SVM consumers; Sui listener needs an emit-shape change plus a topic rename.

## Morning review

- done: Task 1 — payload inventory complete with the shape matrix.
- pending: Task 2 (option scoring, in flight), Task 3 (decision note).
- blocked-on: Option A renames `blockchain_listener.crypto_deposit` to match `sweeping-{ENV}-deposits` — is breaking the existing Sui topic name acceptable, or must the listener dual-publish during migration? Need your call before drafting the decision note.
- notes: read-only flight — no project files touched; all findings staged in flight notes pending the gate.
