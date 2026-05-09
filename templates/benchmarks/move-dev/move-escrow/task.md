---
name: move-escrow
description: Atomic two-party NFT swap with shared object lifecycle and capability-gated cancel.
---

# Move Escrow Benchmark

## Goal

Implement an atomic two-party NFT escrow on Sui Move 2024. Two parties
agree on a swap: party A locks `NFT_A`, party B locks `NFT_B`. Once
both NFTs are locked, either party can call `complete_swap` and the
NFTs are transferred to their counterpart. Either party may cancel
their own side before the swap completes; cancelling refunds the
canceller's NFT to themselves.

The module should expose:
- `create_escrow(nft_a, partner: address, ctx)` — party A locks their
  NFT, declares the partner address, creates a shared `Escrow` object,
  and gets back an `EscrowCap` proving they're the originator.
- `accept_escrow(escrow, nft_b, cap, ctx)` — party B locks their NFT
  into the escrow, holding their own `EscrowCap` proving they were
  the named partner.
- `complete_swap(escrow, ctx)` — public; only callable when both NFTs
  are locked. Transfers them to the opposite party.
- `cancel(escrow, cap, ctx)` — capability-gated. The holder's locked
  NFT (if any) is returned. Aborts with `EUnauthorized` if the cap
  doesn't match the escrow.

## Constraints

- Module must compile with `sui move build` cleanly.
- All tests under `tests/` must pass with `sui move test`.
- The `Escrow` is a shared object (multi-party access).
- `EscrowCap` is `key, store`; one cap per party; never embedded inside
  the escrow.
- Cancel-by-stranger MUST abort. There must be a test for this with
  `#[expected_failure(abort_code = ...)]`.
- All abort paths use named `const E*` error constants.
- Use `transfer::public_transfer` for cross-module NFT transfers (NFTs
  are caller-supplied with `key + store`).

## What to deliver

- `Move.toml` with `edition = "2024.beta"` or newer, depending on
  `Sui = { local = "..." }` or by-version.
- `sources/escrow.move` — the module.
- `tests/escrow_tests.move` — at minimum:
  - happy path (both parties accept → `complete_swap` succeeds; verify
    NFTs ended up in the right addresses)
  - cancel-by-originator before partner accepts → originator gets NFT
    back
  - `#[expected_failure]` on cancel by an unauthorized party
  - `#[expected_failure]` on `complete_swap` before both sides locked

The evaluator will score against the move-dev rubric. Categories that
matter most: Capabilities, Aborts and errors, Transfer semantics, Test
coverage.
