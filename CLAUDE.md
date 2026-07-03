# Quorum — Claude Code Instructions

**Multi-Domain Agent Orchestration Daemon.** A deterministic C++20 daemon orchestrates AI agents that coordinate through HANDOFF blocks and persist knowledge in local vaults. The daemon spawns `claude -p` (Claude Code CLI, non-interactive) as the agent runtime. Pure local orchestration on a single machine; web3 (Sui/Walrus/Seal) deferred indefinitely.

> **Project status, roadmap, phases, and design rationale live in the maintainer's private design vault — NOT in this repo.** This file is what an agent needs to *write code here*. For "what is the current phase / why," ask the maintainer or check the vault. **The source is the truth** — read the headers rather than relying on restated detail here (it drifts).

## Two modes (per conversation)

- **generic** (default) — agents mutate the project; doers write real artifacts in their `target_dir`.
- **brainstorm** (`--mode brainstorm`) — every agent clamped read-only at the tool layer (invoker overrides `agent_class` to analyst); knowledge writes are human-gated (daemon-enforced): the leader presents a per-knower manifest, and on approval the participating knowers self-write their own `ref-*.md` vault slices.

Same agents, same HANDOFF protocol, sequential dispatch in both. Only the write surface differs.

## Build, Test, Run

```bash
make build               # cmake + compile (Release) → build/quorum_daemon
make test                # build + ctest --output-on-failure
make build-debug         # debug symbols
make install             # symlink `quorum` CLI → ~/.local/bin + role skills + supervisor agent (make uninstall removes)
make run-verbose         # run daemon with verbose logging
./scripts/web.sh start   # web dashboard in background (UI :3101, API :3100)
```

## C++ Conventions (hard constraints)

- **C++20**, namespace `sui::quorum`, header-only where practical (daemon logic in `quorum-core/src/**/*.h`)
- **No Boost. No JSON library** — manual `utils/json.h`; **manual YAML** — `utils/config.h`
- **No exceptions for control flow** — return `bool` / `std::optional<T>`; **RAII** for all resources
- Prefer `std::string_view` (read-only) / `std::string` (owning), `std::filesystem`, structured bindings
- `snake_case.h/.cpp`, tests `test_<module>.cpp`, configs `snake_case.yaml`; timestamps = epoch seconds `uint64_t`; logging = stdout/stderr only
- Runtime deps: sqlite3, OpenSSL, libcurl, and the `claude` CLI (installed + authenticated). Reference codebase for C++ idioms: `../bot-manager`.

## Layout

`quorum-core/src/` — the daemon: `main.cpp` (CLI parse + dispatch + daemon loop) · `daemon/` (conversation engine, scheduler) · `agent/` (invoker, output_parser, context_assembler, rubric) · `storage/` (SQLite + `schema.h`) · `vault/` (per-agent vaults; `vault_manager.h`, `indexer.h`, `retention.h`, `context_history.h`) · `cli/` (one header per `quorum <subcommand>`) · `utils/`. `templates/` — agent CONTEXT.md, role/craft skills, `specs/` (autopilot protocol). `quorum-web/` — Bun+Hono API (:3100) + React UI (:3101). `scripts/` — install-skills, web, setup-knowers, lint/update-templates.

## Architecture invariants

- **Zero LLM in the control loop.** The daemon never calls an LLM; all scheduling/routing/events are pure C++. LLMs run only in `claude -p` subprocesses spawned by the task-dispatch loop.
- **Role → tool class:** `doer` = executor (full tools, `target_dir` cwd); every other role = analyst (`--disallowedTools "Write,Edit,NotebookEdit"`). Analyst roles "write" by emitting structured blocks the daemon applies — they never hold Write/Edit at runtime.
- **Four core roles:** leader, thinker, doer, evaluator. (evaluator = "is it *good*?" → rubric score; correctness/convention checks — "does it work?" — are a thinker-role review specialty or the doer itself, not a core role.) The read-only **knowers** (cartographer / architect / historian / recap) and **advisor** are thinker-role specialties. Plus the **supervisor** — an installed agent definition (the autopilot driver), started `claude --agent supervisor` (interactive, not a daemon worker).
- **Sequential dispatch** in the daemon engine — one `claude -p` at a time; per-task token cap + window budget; crash recovery re-dispatches stale `active` tasks on startup.
- **Structured output blocks** parsed by `agent/output_parser.h`: HANDOFF, VAULT_UPDATE, PROPOSAL, REVIEW, OBSERVATION, SUMMARY, EVALUATION. Read the parser for exact field shapes — don't restate them.

## Key idioms

```cpp
// JSON (manual, no libs)
auto text = sui::quorum::json::extract_string(json_str, "result");
// SQLite (WAL, mutex, RAII; schema in storage/schema.h)
sui::quorum::Database db("quorum.db");
db.execute("CREATE TABLE IF NOT EXISTS metrics (...)");
```

Project-local `.quorum/` (created by `quorum init`): `config.yaml`, `quorum.db` (schema from `storage/schema.h`), `agents/*.yaml` (auto-discovered), `vaults/<agent>/` (CONTEXT.md + knowledge/). CLI auto-discovers `.quorum/` by walking up from cwd — no `--config` needed except to run the long-lived daemon.

Knowledge layer: `quorum knower refresh` re-surveys the codebase into the knower vaults; `quorum ask` gives an LLM answer from them + live code; `quorum search "<q>"` is the deterministic $0 (no-LLM) ranked search over `ref-*.md`.

## Testing

- Unit tests in `quorum-core/tests/unit/`, integration in `tests/integration/`; each is a standalone `int main()` returning 0/1, registered in `CMakeLists.txt` via `add_test`.
- Run one: `cd build && ctest -R test_name`. Prefer real `/tmp` dirs over mocks.

## What NOT to do

- Don't add external dependencies (no Boost, no JSON/YAML libs, no ORM, no framework)
- Don't break the "zero LLM in control loop" invariant — the daemon stays deterministic
- Don't let analyst agents write files — enforced via `--disallowedTools`
- Don't bypass the HANDOFF protocol for routing
- Don't edit `.quorum/` runtime data by hand (let the daemon manage it)
- Don't put project status / roadmap here (lives in the vault)

## Known issues

- `claude -p` refuses to launch inside another Claude Code session (`CLAUDECODE` env var) — run from a regular terminal.
- Buffered stdout when redirected to a file — add `std::flush` for real-time tailing.
