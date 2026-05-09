# move-escrow — evaluator notes

Medium-difficulty benchmark. A well-prepared move-dev agent should
clear 60+. Items to weight more heavily than the default rubric:

- **Capability discipline** — privileged ops MUST require `&Cap`, not
  `ctx.sender()` comparisons. The unauthorized-cancel test is the
  acid test; a module that lets stranger-cancel succeed silently is
  fundamentally broken regardless of compile/test counts.
- **Fail-closed semantics** — `complete_swap` before both NFTs are
  locked must abort BEFORE any state mutation. A module that completes
  half-swap then aborts has lost an NFT.
- **Test coverage — authorization-failure tests** — the
  `#[expected_failure]` test for stranger-cancel is mandatory. A
  module without it can't be trusted regardless of how the happy path
  looks.

Items that matter LESS for this task:
- Comments and docs — care more about the test discipline than the doc
  prose for this benchmark.
- Hot-potato discipline — escrow holds shared objects, not hot-potato
  flows; the relevant rubric items are mostly N/A here.
