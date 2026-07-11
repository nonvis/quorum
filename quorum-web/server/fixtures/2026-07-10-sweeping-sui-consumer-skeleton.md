# Autopilot checkpoint

Created at: 2026-07-09T21:14:02Z
Updated at: 2026-07-10T05:41:37Z
Flight spec: 0.3
Fixture: true (synthetic demo flight — safe to delete)
Mode: generic
Goal: Wire the sweeping-sui Kafka consumer skeleton end-to-end

## Major tasks

- [x] Task 1: Consume the deposits topic — add a Kafka consumer module to sweeping-sui that subscribes to the deposits topic and validates payloads against HistoryDto
- [x] Task 2: Lease + compose — persist consumed deposits, take a sweep lease, and compose (but do not submit) the sweep PTB
- [x] Task 3: Register the scheduler — register SchedulerSUIService in application.module.ts and smoke-test the interval trigger

## Condensed outcomes

### Task 1 — Kafka consumer module
- Consumer module added behind the existing Fastify/tRPC scaffold; subscribes via the shared kafkajs config and validates each message against `HistoryDto[]` (batched shape), dead-letters malformed payloads with the payload logged.
- Verdict: consume path compiles and unit tests pass (14 new tests); the single-event `DepositEvent` shape from sui-listener is NOT accepted — flagged for the wire-contract decision, consistent with the architect's coupling note.

### Task 2 — lease + compose
- Deposits persist to the service DB with `FOR UPDATE SKIP LOCKED` lease semantics mirroring the publisher side; PTB composition builds `sweep_coin` calls from leased rows and serializes them for a future submitter.
- Verdict: compose-only path verified by round-trip test (compose → decode → field match); no on-chain submission was wired, by design.

### Task 3 — scheduler registration
- SchedulerSUIService registered in application.module.ts; interval trigger fires and enqueues a lease sweep on the dev profile.
- Verdict: registration done; live smoke-test SKIPPED — no localnet was available in the flight environment, so the trigger was verified against a mocked queue only.

## Morning review

- done: Tasks 1–3 — consumer, lease/compose, scheduler registration all landed with tests.
- pending: none — flight plan complete.
- blocked-on: none
- notes: scheduler live smoke-test was SKIPPED (no localnet in flight env) — worth a manual pass. The consumer deliberately rejects sui-listener's single-event shape; the wire-contract unification decision is still open.
