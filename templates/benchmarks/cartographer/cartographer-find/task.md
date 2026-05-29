---
name: cartographer-find
description: Build a layout index and answer fast "where is X" lookups for a small TS workspace.
---

# Cartographer Find Benchmark

## Goal

This workspace holds a small TypeScript project with a couple of top-level
packages and a docs folder. First, build a structured layout index: for each
top-level folder, its one-line purpose and its key files (entry point, build
manifest, config). Then answer these orientation lookups directly from your
index — the kind a teammate fires off in chat:

1. Where is the package manifest for the `api` package?
2. Where is the TypeScript config?
3. Which folder holds the docs, and what is the doc entry file?

## Constraints

- Index the TOP level only. Do not enumerate every file.
- The index must be a STRUCTURED form (table / keyed records), so the three
  lookups are answered by reading the index, not by re-scanning the tree.
- Do NOT reason about how the packages depend on each other — that is the
  architect's job.
- Every cited path must exist on disk. Do not fabricate files or folders.
- Include a staleness stamp.

## What to deliver

- A structured layout index of every top-level folder with purpose + key
  files.
- Direct answers to the three lookup questions, each citing a real path.

The evaluator scores this against the cartographer rubric. The filesystem is
the ground truth; lookup correctness + structure are weighted here.
