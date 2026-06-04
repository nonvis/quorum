# cpp-test-coverage — evaluator notes

This task is exclusively about test discipline. Items to weight more heavily:

- **Testing** — every public function should have a happy-path test AND a
  failure test where a precondition/authorization gate exists. Boundary cases
  (empty, 1-byte, 32-byte, 33-byte, missing lookup) on top. Score this category
  strictly; it is what the benchmark measures. A stub-only or thin suite that
  skips the failure paths is a FAIL on the coverage item, not partial credit.
- **Error handling — assert the named error** — the failure tests must assert
  the specific `enum class` error value (e.g. `RegistryError::NotOwner`), not
  just "an error occurred" and not a raw integer. A test that only checks a
  bool/`!= Ok` is sloppy — dock the relevant item.
- **Compilation and build** — full pass under `ctest` is the binary correctness
  floor. Don't give partial credit if some tests fail; one red test means the
  agent's understanding of the component's behavior is wrong.

Self-transfer behavior is intentionally underspecified in the task to test
whether the agent **reads the source first** or just guesses. Check the starter
source for the actual behavior and confirm the agent's test matches it — reward
the agent that read, penalize the one that asserted the wrong behavior.

Items that matter LESS:
- Modern C++20 idioms — the source is provided and must not be modified; the
  agent writes tests. The tests should still be clean (no raw `new`, descriptive
  names), but idiom depth isn't the focus.
- Concurrency and determinism — single-threaded component; the determinism
  *items* are N/A, but the tests themselves must be deterministic (no sleeps).
- Memory and resource safety — minimal surface in test code; don't over-weight.
