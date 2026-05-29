---
name: cartographer
version: v1
---

# Rubric: cartographer (v1)

Source: Quorum Specialties note "04 - Cartographer Quality Bar" (second-brain
vault). The cartographer is a thinker (analyst-class, read-only) that knows a
project's LAYOUT — what each top-level folder/repo contains, and where the
important files/subfolders live — and serves orientation questions instantly.
It is the shallowest, cheapest "knower"; it never reasons about
interconnections (that is the architect).

The filesystem is the ground truth, so ~75% of this bar (coverage + content
accuracy + key-file/location accuracy) is mechanically checkable: the evaluator
(analyst, Read/Grep/Glob) walks the index against the actual workspace. This is
the cleanest precision/recall calibration of the three knowers.

The `evaluator` agent (Phase 8 Track 1) reads this file, walks each item, and
emits per-item pass/fail in the EVALUATION block (Phase 8 Track 3). Categories
are documentation; per-item `(W)` weights drive scoring.

## Top-level coverage (weight 25)
- [ ] (10) Every top-level folder/repo in the workspace root appears in the index — none missed (verify against `ls` of the root / `--maxdepth 1`)
- [ ] (8) No folder/repo is fabricated — every indexed top-level entry exists on disk
- [ ] (4) A workspace `CLAUDE.md` (if present) is read first and its folder descriptions are treated as authoritative, with code-vs-description contradictions flagged
- [ ] (3) Top-level files that are orientation anchors (root README, workspace manifest, `CLAUDE.md`) are noted, not just folders

## Per-folder content accuracy (weight 25)
- [ ] (10) Each top-level folder has a one-line purpose statement that correctly describes what it contains (verify by opening the folder)
- [ ] (8) The stated "kind" of each component (service / contract / listener / config / library) matches what the folder actually holds
- [ ] (4) Purpose statements are concise — one line, not a paragraph dump or a file listing
- [ ] (3) No content claim contradicts the folder's own README / manifest / `CLAUDE.md`

## Key-file / location accuracy (weight 25)
- [ ] (9) Every cited key-file path exists on disk at the stated location (entry points, build manifests, config)
- [ ] (8) Each cited file actually holds what the index claims (e.g. the named manifest is a manifest, the named entry point is an entry point)
- [ ] (5) Build/dependency manifests are located for each component that has one (`package.json` / `Cargo.toml` / `Move.toml` / `go.mod` / Solidity sources / etc.)
- [ ] (3) The "if you need X, look here" anchors (config, entry point) are present for components that have them

## Right altitude (weight 10)
- [ ] (5) Indexes top-level structure only — does not drown in every file in the tree
- [ ] (3) Granularity is useful — not so vague that a folder's purpose is unidentifiable
- [ ] (2) Does NOT cross into interconnection reasoning ("how A uses B") — that is deferred to the architect

## Lookup structure & speed (weight 10)
- [ ] (5) The index is a structured form (table / keyed records), not free prose, so "where is X" is answerable by lookup
- [ ] (3) A direct "where is X?" / "what's in folder Y?" query is answered correctly straight from the index, without re-scanning the tree
- [ ] (2) Entries are keyed/ordered so a specific component or file is findable without reading the whole index

## Freshness + clarity (weight 5)
- [ ] (3) A staleness stamp is present (indexed-against date / commit) so drift is visible
- [ ] (2) Entries are clear and unambiguous — a reader can act on each without follow-up
