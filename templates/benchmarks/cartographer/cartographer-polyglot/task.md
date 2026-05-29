---
name: cartographer-polyglot
description: Index the layout of a small polyglot workspace per the cartographer bar.
---

# Cartographer Polyglot Benchmark

## Goal

Index the layout of the workspace in the project root. This is a small
polyglot workspace with several top-level components: produce a layout
index that says, for each top-level folder, what it contains (a one-line
purpose) and where its important files live (entry point, build manifest,
config). Answer the orientation question a teammate would ask first:
"what's in here and where do I look?"

Then answer these two lookup questions from your index:
1. Where is the build manifest for the Rust service?
2. Which top-level folder holds the on-chain contract, and what is its
   manifest?

## Constraints

- Stay at the TOP level — index each top-level folder/repo, not every
  file in the tree. Do not produce a 500-file dump.
- Do NOT reason about how the components interconnect (which service calls
  which) — that is the architect's job. Defer any interconnection question.
- If a `CLAUDE.md` is present, read it first and honor its folder
  descriptions as authoritative; flag any place the code contradicts it.
- Every cited file path must actually exist — do not fabricate folders,
  files, or manifests.
- The index must be a STRUCTURED form (a table or keyed records), not a
  prose paragraph, so a "where is X" lookup is answerable directly.
- Include a staleness stamp (the date / commit you indexed against).

## What to deliver

- A structured layout index covering every top-level folder, each with:
  its one-line purpose, its kind (service / contract / config / docs),
  and its key files (entry point, build manifest, config).
- Direct answers to the two lookup questions above, served from the index.

The evaluator will score this against the cartographer rubric. The
filesystem is the ground truth — coverage, content accuracy, and
key-file/location accuracy are checked directly against the actual layout.
