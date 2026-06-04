# ts-test-coverage — evaluator notes

Exclusively about test discipline. Items to weight more heavily:

- **Testing** — every exported function gets a happy-path test AND a failure test
  where a gate exists; boundaries (empty / 1 / 32 / 33 chars, missing lookup) on
  top. Score strictly; a thin suite that skips the failure paths is a FAIL on the
  coverage item, not partial credit.
- **Type safety** — failure tests assert the **specific** exported error value
  (e.g. `RegistryError.NotOwner` / the returned discriminated result), not a bare
  string or `toBeTruthy()`. No `any` in the test file.
- **Tooling and build** — `tsc --noEmit` clean + full `vitest run` green is the
  binary floor. One red test means the agent's understanding of the module is
  wrong; no partial credit.

Self-transfer is intentionally underspecified in the task to test whether the
agent **reads the source first** or guesses. Check `src/registry.ts` for the
actual behavior (it succeeds as a no-op) and confirm the agent's test matches —
reward the one that read, penalize the wrong assertion.

Items that matter LESS:
- Sui SDK correctness / dApp Kit — N/A.
- Structure — the agent writes tests, not module code; clean test structure
  matters but idiom depth isn't the focus.
