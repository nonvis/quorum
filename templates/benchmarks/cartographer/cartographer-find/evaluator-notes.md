# cartographer-find — evaluator notes

The fixture workspace (from `expected/`) is the ground truth. Score the index +
the three lookup answers against the actual filesystem. This benchmark stresses
**lookup structure & speed** and **key-file/location accuracy** — the answers
must be served from a structured index, not narrated from a fresh scan.

## Ground-truth layout

Top-level entries at the workspace root:

| Entry | Kind | Purpose (ground truth) | Key files |
|-------|------|------------------------|-----------|
| `api/` | package (TS) | HTTP API entry point; depends on `core` | manifest `api/package.json`; entry `api/src/index.ts` |
| `core/` | package (TS) | shared utilities | manifest `core/package.json`; entry `core/src/index.ts` |
| `docs/` | docs | workspace documentation | `docs/README.md` |
| `tsconfig.json` | config (root) | TypeScript build config for both packages | (root file) |

Exactly THREE top-level folders (`api`, `core`, `docs`) plus a root
`tsconfig.json`. There is no `CLAUDE.md` in this fixture, so the
CLAUDE.md-honored item should be scored N/A (no contradiction to flag, no
authoritative description to seed from).

## Lookup answers (ground truth)

1. `api` package manifest → `api/package.json`.
2. TypeScript config → `tsconfig.json` (root).
3. Docs folder → `docs/`; doc entry file → `docs/README.md`.

## Scoring emphasis

- **Lookup structure & speed (10)** — full weight here. Each of the three
  answers must be a correct real path AND be the kind of answer a structured
  index produces directly. An index that is prose, forcing a re-scan to answer,
  fails the structure item even if the paths are right.
- **Key-file/location accuracy (25)** — `api/package.json`, `core/package.json`,
  `api/src/index.ts`, `core/src/index.ts`, `tsconfig.json`, `docs/README.md`
  are all real. Any fabricated path (e.g. a non-existent `api/tsconfig.json` or
  a root `package.json`) fails the relevant item.
- **Right altitude** — do not enumerate every `.ts` file; folder + key files is
  the target. Do not describe the `api` → `core` dependency as an
  interconnection map — noting `core` is a workspace dep of `api` from the
  manifest is fine as a content fact, but tracing call relationships is the
  architect's job and out of altitude here.

## Items that matter LESS

- Freshness + clarity (5) — small dock for a missing staleness stamp.
