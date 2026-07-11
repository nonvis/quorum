# quorum-own-agent

A hand-rolled, stdlib-only AI agent that answers questions strictly from a
Quorum project's accumulated knowledge base — the knower vaults under
`.quorum/`. No framework, no agent SDK, no native tool-calling API. The point
is pedagogical: every mechanism an "agent" is made of — retrieval, the tool
protocol, the ReAct loop, the model seam, grounding, citations — is written out
in plain Python where you can read it.

## Where it sits

This fills the gap between the two things Quorum already gives you:

- `quorum search` — deterministic keyword rank. $0, instant, but no synthesis:
  you get chunks, you read them yourself.
- `quorum ask` — full Claude with the whole vault as context. Synthesizes and
  cites, but costs tokens and takes minutes.

`quorum-own-agent` is the middle rung: semantic-ish retrieval (BM25 over an FTS5
index) plus synthesis plus citations, in seconds, and $0 once the brain is a
local model. Good enough to answer "what did we decide about X" without paying
for a full `ask`.

## Architecture

```
   question
      │
      ▼
  ┌─────────┐   ACTION: search/read/map(...)   ┌───────┐
  │  brain  │ ──────────────────────────────►  │ tool  │
  │ (model) │ ◄──────────────────────────────  └───────┘
  └─────────┘        OBSERVATION appended
      │  │            (transcript grows, brain called again)
      │  └──────────────────► loop back until ANSWER or budget
      │
      ▼  ANSWER: … [path citation]
    answer
```

| File | Responsibility |
|------|----------------|
| `ownagent.py`  | CLI entry — `index` / `map` / `search` / `ask` / `eval` subcommands |
| `indexer.py`   | Corpus discovery, markdown-heading chunking, the SQLite FTS5 index |
| `retrieval.py` | Retrieval over the index — the agent's three tools live here |
| `brains.py`    | The Brain seam — the one model interface the rest depends on |
| `prompts.py`   | Prompt templates — the text protocol that makes the brain swappable |
| `loop.py`      | The hand-rolled ReAct loop (`run_agent`) + the v0 `single_shot` pipeline |
| `goldeval.py`  | Golden-set eval harness (substance + citation scoring) |

## The Brain seam

`Brain.complete(prompt) -> text` is the **only** model dependency in the whole
project. Everything agentic is built around it, so the model swaps without
touching the harness.

| Backend | How it reaches the model | Notes |
|---------|--------------------------|-------|
| `ClaudeCLIBrain`   | shells `claude -p <prompt>` | No `--model` flag by default, so it tracks whatever the operator's CLI default is. Strips `CLAUDECODE` env (same nesting guard the daemon uses). |
| `LocalServerBrain` | `POST /v1/chat/completions` to any OpenAI-compatible server | e.g. `llama-server`, LM Studio, `mlx_lm.server`. Point `--base-url` at it and the same agent runs $0/offline. |

The deliberate choice here is a **text protocol** (ACTION/ANSWER lines) instead
of a native tool-calling API. That keeps the harness backend-agnostic — a
7B local model that has never heard of function-calling can still drive it — and
it forces you to learn the real mechanics: parsing tool calls out of prose,
nudging on malformed replies, step budgets, forcing a final answer.

## The text protocol

The brain must reply with exactly one directive per turn:

```
THINK: <one short line>            (optional, may precede an ACTION)
ACTION: search("<keywords>")       BM25 search over knowledge chunks
ACTION: read("<path>")             full text of one note (path as shown)
ACTION: map()                      list every note with owner + summary
ANSWER: <text, every claim cited with the note path in [square brackets]>
```

Rules baked into the preamble: reply with **either** one `ACTION` line **or**
one `ANSWER`, never both. Cite every claim with its `[path]`. If the knowledge
base does not contain the answer, say exactly that (and say what you searched) —
never invent a fact or a citation. Budget of `max_steps` actions; on exhaustion
the loop forces a grounded final answer from the observations gathered so far.

## Usage

```
python3 ownagent.py index  --project <root>
python3 ownagent.py map    --project <root>
python3 ownagent.py search --project <root> "<keywords>" [-k N]
python3 ownagent.py ask    --project <root> "<question>" [--single-shot]
python3 ownagent.py eval   --project <root> [--golden FILE] [--agentic]
```

Against a real project:

```
python3 ownagent.py index  --project /Users/sangsoo/projects/bastion
python3 ownagent.py map    --project /Users/sangsoo/projects/bastion
python3 ownagent.py search --project /Users/sangsoo/projects/bastion "sweep contract factory" -k 8
python3 ownagent.py ask    --project /Users/sangsoo/projects/bastion "How does the RegistryFactory authorize a customer sweep?"
python3 ownagent.py ask    --project /Users/sangsoo/projects/bastion "What is the UpgradeCap emergency authority?" --single-shot
python3 ownagent.py eval   --project /Users/sangsoo/projects/bastion --agentic
```

`ask` defaults to `--brain claude`; add `--brain local --base-url <url>` to run
on a local model. `--quiet` prints only the answer (step logs go to stderr).

## Grounding rules

The agent answers **only** from retrieved observations, cites each claim with
its `[path]`, and refuses when the knowledge base doesn't cover the question.
This is not decoration. With a small local model the grounding is what makes it
usable: the model isn't smart enough to be trusted from its own weights, but it
is smart enough to summarize and cite text you hand it. Retrieval + a refusal
rule turns a weak model into a reliable librarian for your own notes.

## The ladder

- **v0 — single-shot pipeline** (`loop.single_shot`). Retrieve once, synthesize
  once. *SHIPPED.* The cheap baseline the loop has to beat.
- **v1 — hand-rolled ReAct loop** (`loop.run_agent`). Multi-step search → read →
  answer over the text protocol. *SHIPPED.*
- **v2 — local brain** via `--brain local`. The seam and `LocalServerBrain` are
  *SHIPPED*; the model rung is *PARKED* (operator call, 2026-07-11): with a paid
  Max subscription the marginal Claude cost is zero, and Docent's speed lives in
  the harness, not the model. Parked, not rejected — the revival triggers (a
  metered billing regime, window pressure, offline/hard-privacy work, harness
  hardening, LoRA distillation) are logged in the vault: Quorum/Docent/02 -
  Design Decisions, D11. Revival: `brew install llama.cpp`, serve any
  OpenAI-compatible model, point `--base-url` at it.
- **v3 — optional/future.** Embeddings for hybrid retrieval (sqlite-vec /
  sentence-transformers) and/or a from-scratch inference toy. *Not built.*

## Eval

Golden questions live at `golden/<project>.jsonl` (resolved from the project
name; override with `--golden`). One JSON record per line:
`{"id": "...", "q": "...", "expect_any": ["substr", ...], "expect_cite": ["note.md", ...]}`
— an answer passes when it contains any `expect_any` substring AND cites any
`expect_cite` path fragment. Exits 0 only if every question passes. `--agentic` grades the loop; default grades single-shot.

```
python3 ownagent.py eval --project /Users/sangsoo/projects/bastion --agentic
```

The rule: **the agentic loop must beat single-shot on the golden set before it
earns promotion.** Promotion means wiring this agent in as a Quorum invoker
option (the daemon's option 2), and that is gated on golden-set results.
Regardless of outcome, doers and leaders stay on Claude — the local agent is a
fast lookup path, not a replacement for the real work.


## Design rules

- **Stdlib only.** `sqlite3`, `urllib`, `subprocess`, `re` — nothing to `pip
  install`. If you can't read it, it isn't here.
- **Index lives inside the target project** at `.quorum/own-agent/index.db`.
  Nothing is written elsewhere.
- **Incremental mtime reindex** runs on every `ask` / `search` / `map`, so the
  index is always fresh without a separate build step.
- **The `read` tool is path-jailed** to `.md` files under the project's
  `.quorum/` — it refuses anything outside that root or with a different suffix.
