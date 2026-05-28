# Scribe write-discipline guide

> Floor rule for any scribe vault write or `.quorum/learnings.md` append.
> Lifted from Suiperpower CLAUDE.md skill authoring rules.
> Companion to `templates/specs/handoff-protocol.md`.

## Source-of-truth rule

Scribe notes must be grounded in real, current sources. You are not allowed to
invent API names, function signatures, package names, version numbers, sponsor
program details, RPC methods, contract addresses, or behavior.

Before writing or editing any technical claim in a scribe note:

1. The author provides the source (URL, pasted docs, GitHub repo path, or a
   file in this repo).
2. You read or fetch that source. If a URL is provided and you can fetch it,
   fetch it. Pasted text is canonical for the turn.
3. You only write claims the source explicitly supports. If the source does
   not cover a claim, drop the claim or ask.
4. If the author has not yet given a source for a topic the note needs,
   **stop and ask**. Sui, Walrus, DeepBook, Scallop, zkLogin, Seal, Nautilus
   and similar topics drift fast; outdated training data is the most common
   source of bad notes.
5. Prefer canonical sources: official docs, official GitHub README, official
   reference documentation. Avoid third-party tutorials or AI-generated
   summaries as primary references.

**If a source contradicts an earlier-saved memory, the source wins.**

## Common mistakes to avoid

- **Inventing function names or APIs** that sound plausible but do not exist.
  Especially with Sui SDK methods, Move stdlib, Walrus / DeepBook / Scallop
  calls. Always verify against source.
- **Stale package names or versions** (`@mysten/sui.js` is old, current is
  `@mysten/sui`).
- **Confusing Sui Move with Aptos Move or Core Move**. Dialects diverge.
- **Outdated sponsor program details** (track names, prize structure, judging
  criteria, deadlines).
- **Pasting full doc pages into the note body** instead of linking.
- **Adding "best practices" that are actually opinions**, not sourced.
- **Marketing voice creeping in** ("seamlessly", "powerful", "robust",
  "leverage"). Cut on sight.
- **Em-dashes** (project-wide ban).
- **Hand-waving the unknown**: if you do not know, say "ask the author" or
  "fetch X at runtime", do not fill the gap with confident guesses.

## Voice

- Senior-friend, direct.
- No marketing copy.
- No emojis.
- Plain ASCII; UTF-8 only when source content requires it.
