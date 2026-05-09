# Quorum — Claude Code Instructions

## Project Overview

Quorum is a **multi-agent orchestration framework**. A deterministic C++20 daemon orchestrates independent AI agents that coordinate through structured HANDOFF blocks and persist knowledge in local vaults.

Every conversation runs in one of two **modes**:
- **generic** (default) — agents mutate the project. Doers write real artifacts in their `target_dir`.
- **brainstorm** — every agent is clamped read-only at the tool layer (invoker overrides `agent_class` to analyst). Scribe at end of cycle distributes curated knowledge files (`rule-*.md`, `ref-*.md`) across all agent vaults.

Sequential dispatch, HANDOFF protocol, and the agent roster are identical in both modes. Only the write surface differs.

Pure local orchestration on a single MacBook. The daemon spawns `claude -p` (Claude Code CLI in non-interactive mode) as the agent runtime. Web3 layers (Sui, Walrus, Seal) are deferred indefinitely.

> Phase status, roadmap, design decisions, and architectural rationale live in the project's design vault (private to the maintainer). This file documents repo-specific conventions an AI agent needs to write code in this codebase. For "what is Quorum" / "how does it work" / "what's the current phase," ask the maintainer or check the vault.

## Repo Layout

```
quorum/
├── CLAUDE.md                    <- You are here
├── quorum-core/                 # C++20 (CLOSED SOURCE) — orchestrator daemon
│   ├── src/
│   │   ├── main.cpp             # Daemon entry, signal handling, PID lock, CLI subcommands
│   │   ├── daemon/              # Scheduler, message bus, events, conversation engine
│   │   ├── agent/               # Claude Code invoker, context assembler, output parser
│   │   ├── vault/               # Local vault manager (filesystem-based)
│   │   ├── storage/             # SQLite (WAL mode) — schema.h, database.h, local_cache.h
│   │   ├── utils/               # HTTP, JSON (manual), config, UUID, subprocess, discover
│   │   └── cli/                 # CLI commands (init.h, agent_create.h, skills.h)
│   └── tests/
├── quorum-web/                  # Web dashboard (Hono API + React frontend)
│   ├── config.ts                # Dynamic project config, state persistence
│   ├── server/                  # index.ts (routes), db.ts, daemon.ts (PID check), sse.ts
│   └── client/src/              # App.tsx, api.ts, types.ts, components/
├── templates/                   # Single source of truth for all templates
│   ├── skills/                  # SKILL.md files (behavioral + domain)
│   │   ├── quorum-roles/        # Role skills (leader, thinker, doer, scribe, reviewer)
│   │   ├── sui-dev-skills/      # Sui domain skills (sui-move, sui-ts-sdk, sui-frontend)
│   │   └── move-code-quality/   # Move code quality checklist
│   └── agents/                  # CONTEXT.md templates with {placeholders}
├── scripts/                     # Utility scripts
│   ├── install-skills.sh        # Install skills to ~/.claude/skills/ (copy or --link)
│   ├── lint-templates.sh        # Validate template consistency
│   └── update-templates.sh      # Review templates via claude -p
├── .claude/commands/            # Claude Code skills (project scaffolding)
└── docs/                        # Design documents
```

## Reference Codebase

The bot-manager project at `../bot-manager` is the primary C++ reference. Reuse its patterns for HTTP, JSON, crypto, SQLite, and general C++ style.

## Hard Constraints

### C++ Conventions

- **C++20 standard** — concepts, ranges, designated initializers, std::format
- **Namespace:** `sui::quorum`
- **No nlohmann/json** — use manual `json.h` parsing (see bot-manager)
- **No Boost** — ever
- **No exceptions for control flow** — use return codes / std::optional / std::expected
- **Header-only preferred** for new modules
- **Timestamps:** epoch seconds as `uint64_t`
- **Logging:** stdout/stderr only
- **String handling:** `std::string_view` for read-only, `std::string` for ownership
- **Error handling:** return `bool` or `std::optional<T>` — never throw

### Dependencies

| Dependency | Purpose |
|-----------|---------|
| sqlite3 | Task queue, token tracking, conversations, knowledge, budget |
| OpenSSL | Hashing (SHA-256), TLS |
| libcurl | HTTP client |

**Runtime dependency:** `claude` CLI (Claude Code) must be installed and authenticated.

**Banned:** nlohmann/json, Boost, any JSON library, any ORM, any framework

### File Naming

- Headers: `snake_case.h` — Sources: `snake_case.cpp` — Tests: `test_<module>.cpp` — Configs: `snake_case.yaml`

## Architecture Rules

### The Daemon Is Deterministic

The daemon **never** calls an LLM. All scheduling, routing, and event handling is pure C++ logic. Agent invocations happen by spawning `claude -p` subprocesses, triggered by the task dispatch loop.

### Agent Invocation

```
daemon -> assembles prompt (vault + task) -> spawns `claude -p` with class-appropriate flags -> parses result -> writes to vault -> routes follow-up tasks
```

**Agent classes** (set in `AgentMetadata`, configured via `role` in agent YAML):
- **`analyst`** (default, all roles except doer): `--disallowedTools "Write,Edit,NotebookEdit"` — read-only
- **`executor`** (doer role): full tool access, `target_dir` sets working directory

**Brainstorm-mode override:** When the conversation's `mode` is `brainstorm`, the invoker overrides `agent_class` to `analyst` for every agent regardless of role. Doers run read-only too. The override happens at the tool layer (`--disallowedTools`), not in agent YAML.

**Per-agent model selection:** Optional `model` field in agent YAML (e.g., `model: sonnet`). When set, adds `--model <value>` to the `claude -p` command. Empty = use claude default.

**Agent archetypes (roles):** `leader`, `thinker`, `doer`, `reviewer`, `evaluator`, `scribe`, `librarian`. All non-doer roles run as `analyst` (read-only); doer runs as `executor`. Reviewer and evaluator are distinct judging roles:
- **`reviewer`** — judges correctness ("does this work?"). Free-form critique against the task spec.
- **`evaluator`** — judges specialty quality ("is this *good*?"). Scores doer output against a structured rubric and emits an EVALUATION block (see Quality Framework below).

**Session management** (conversation mode only):
- First use: `--session-id <uuid>` — Subsequent: `-r <uuid>` (resume)
- Resume fallback: if `-r` fails, retries with `--session-id` (fresh session)
- Task Queue mode (no `conversation_id`): no session flag, fresh context each time

### Agent Output Rules (Defense-in-Depth)

Analysts must produce structured blocks (VAULT_UPDATE, SUMMARY, HANDOFF) — never write files directly. Enforced at three layers:
1. **`--disallowedTools`** — hard enforcement for analyst agents
2. **CONTEXT.md** — per-agent instructions prohibiting file writes
3. **Context assembler** — injected into every prompt as failsafe

Executors are exempt — they write files directly in their `target_dir`.

### Sequential Dispatch

One `claude -p` task at a time, always. Design decision, not configuration.
- Per-task token cap (kills process if exceeded) + window budget (pauses dispatch when exhausted)
- Crash recovery: `recover_stale_tasks()` on startup marks stuck `active` tasks as `failed`, re-dispatches affected conversations to leader

### Vault System

Each agent owns a vault: `{data_dir}/vaults/{agent_name}/`
- `CONTEXT.md` — agent identity + instructions (auto-generated, or editable via web UI)
- `knowledge/` — accumulated findings via VAULT_UPDATE blocks

### Knowledge System (filename-typed, 3-scope hierarchy)

Knowledge files use filename prefixes to signal handling. The context_assembler treats them differently:

- `rule-*.md` — **always-on directives**. Preloaded into every agent prompt. Hard cap `MAX_RULES = 10` across the scope union; oldest evicted with a `[N rules omitted]` note.
- `ref-*.md` — **searchable references**. NOT preloaded. The daemon scores them against the agent's task prompt at assembly time (filename matches × 3 + content matches × 1) and surfaces top 5 in a `## Searched References` section. Agents read full content via the existing Read tool.
- Plain names — narrative summaries. Loaded by recency under remaining budget.

**Scope hierarchy (3 levels):** rules and refs resolve in order — `.quorum/knowledge/` (project) → `.quorum/knowledge/roles/<role>/` (role) → `<vault>/knowledge/` (agent). Most-specific scope wins on identical-content dedup. The MAX_RULES cap operates on the union, sorted by mtime DESC.

**Write paths:**
- VAULT_UPDATE blocks — agents write to their own vault (`path: knowledge/<file>.md`). Persistent.
- Brainstorm scribe — gets the one cross-vault exception (Decision #26): in brainstorm mode, scribe can emit VAULT_UPDATE with `path: <other-agent-id>/knowledge/<file>.md` to curate knowledge for the team.
- Conversation transcripts — read by scribe at end of cycle (via `claude -p` session resume), distilled into vault notes.

### Quality Framework (Phase 8)

Evaluators score doer output against a per-specialty rubric. The framework adds no new top-level daemon component — scoring lives in the standard `output_parser` -> `ConversationEngine` flow, persisted via `Database`.

**Specialty model:** an agent has a `role` (what it does) and a `specialty` (what domain it does it in). The combined `role-specialty` key (e.g., `move-dev`) selects the rubric. Doers carry a specialty in their YAML; evaluators are bound to one specialty and grade only matching doer output.

**Rubric format and storage:**
- Canonical: `templates/rubrics/<role-specialty>/rubric.md` (committed to repo, single source of truth)
- Per-project override: `.quorum/rubrics/<role-specialty>/rubric.md` (project-local, takes precedence)
- Versioned via `rubric_version` field in the rubric front-matter; the version is recorded with every evaluation row.

**EVALUATION block format:** Evaluators emit an EVALUATION block alongside the usual SUMMARY/HANDOFF blocks. `items_json` is a JSON-string field (Track 3 convention — embedded JSON encoded as a string, not a structural block field) so the block parser stays a flat key:value reader.

```
[EVALUATION]
rubric: move-dev
rubric_version: 1
score_total: 78
items_json: "[{\"id\":\"safety\",\"score\":4,\"max\":5,\"note\":\"...\"}, ...]"
notes: free-form evaluator commentary
[/EVALUATION]
```

**`evaluations` table columns:** `conversation_id`, `scored_agent_id`, `evaluator_agent_id`, `role_specialty`, `rubric_version`, `score_total`, `score_json`, `notes`, `created_at`. Indexed on `(role_specialty, created_at)` for benchmark trend queries.

**Active specialty — `move-dev`:**
- Rubric v1: `templates/rubrics/move-dev/rubric.md`
- Supporting skill: `templates/skills/sui-dev-skills/sui-move/SKILL.md` (rubric and skill share criteria so doers and evaluators agree on the bar)

**Benchmark suite:** `templates/benchmarks/<role-specialty>/<task>/` holds a fixed input + expected-shape spec per task. Run via:

```
quorum benchmark --role move-dev              # runs all move-dev tasks
quorum benchmark --role move-dev --task <name>  # single task
```

Each benchmark run is a synthetic conversation: doer produces output, evaluator scores against the rubric, results land in `evaluations` for trend tracking across rubric/skill iterations.

### Prompt Cache (Phase 7 Track 5)

`assemble_split()` returns `{system_prompt, user_message}`:
- **system_prompt** (stable per agent) — CONTEXT.md, SKILL.md, output rules. Passed via `--append-system-prompt-file <tmpfile>` so Anthropic's prefix cache hits across conversations.
- **user_message** (varies per task) — rules, refs, inbox, roster, task description. Passed via stdin.

`tasks.cache_creation_input_tokens` and `tasks.cache_read_input_tokens` columns track cache metrics. `/api/budget/agents` exposes per-agent cache hit ratios.

Legacy `assemble()` is now a shim that concatenates both halves with `\n---\n\n` for back-compat with non-split callers.

### Conversation Mode (Team Mode)

`ConversationEngine` (`daemon/conversation.h`) coordinates agent teams via HANDOFF blocks.

**States:** `active`, `waiting_for_human`, `done`, `closed`, `paused`

**Key types:**
- `ConversationRecord` — in `storage/database.h` (id, goal, state, round, max_rounds, budget_usd, spent_usd, current_agent, path_index, team, **mode**)
- `TeamPreset` — in `utils/config.h` (id, name, default_path)
- `ConversationEngine` — header-only in `daemon/conversation.h`. Methods: `start()`, `on_task_complete()`, `respond()`, `resume()`, `close()`, `recover()`
- `Rubric` / `RubricItem` — in `agent/rubric.h`. Loaded from `templates/rubrics/<role-specialty>/rubric.md` (with `.quorum/rubrics/` override). Used by evaluators and the benchmark runner.
- `EvaluationBlock` — in `agent/output_parser.h` as a `ParsedOutput` field. Populated when the parser encounters an `[EVALUATION] ... [/EVALUATION]` block; `ConversationEngine` persists it to the `evaluations` table.

**Database tables:** `conversations` (with `mode` column — `generic` or `brainstorm`), `tasks` (with conversation_id, session_id, system_prompt, cache_creation_input_tokens, cache_read_input_tokens), `agent_sessions` (per-agent session_id tracking for `claude -p` resume), `budget_window` (singleton, tracks window spend with auto-reset), `evaluations` (Phase 8: rubric-scored doer output — see Quality Framework)

**Cross-vault writes (brainstorm only):** `VaultManager::apply_all_updates_with_context` validates that cross-vault writes (paths like `<other-agent>/knowledge/rule-X.md`) only happen when (a) the writer is a scribe and (b) the conversation mode is `brainstorm`. Other agents' VAULT_UPDATE blocks remain scoped to their own vault in both modes.

## Key Patterns

### JSON (manual — no libraries)
```cpp
auto text = sui::quorum::json::extract_string(json_str, "result");
auto cost = sui::quorum::json::extract_number(json_str, "total_cost_usd");
auto toks = sui::quorum::json::extract_int(json_str, "input_tokens");
auto flag = sui::quorum::json::extract_bool(json_str, "is_error");
```

### SQLite
```cpp
sui::quorum::Database db("quorum.db");  // WAL mode, mutex-protected, RAII
db.execute("CREATE TABLE IF NOT EXISTS metrics (...)");
auto result = db.query("SELECT * FROM metrics WHERE agent = ?", agent_id);
```

### Config (project-local — preferred)
```yaml
# .quorum/config.yaml — agents auto-loaded from .quorum/agents/
daemon:
  data_dir: .quorum
  pid_file: .quorum/quorum.pid
budget:
  window_budget_usd: 100.00
  window_hours: 5
conversations:
  enabled: true
  default_max_rounds: 20
  default_budget_usd: 5.0
  leader: leader
```

### Agent YAML
```yaml
# Analyst (read-only tools)
id: market_analyst
name: "Market Analyst"
role: thinker                    # leader, thinker, doer, reviewer, evaluator, scribe, librarian
agent_class: analyst             # auto-derived from role (doer=executor, all others=analyst)
description: "Analyzes market structure"
vault_path: .quorum/vaults/market_analyst/
context_file: .quorum/vaults/market_analyst/CONTEXT.md
skill_file: ~/.claude/skills/quorum-roles/thinker/SKILL.md  # auto-detected from role
model: sonnet                   # optional: per-agent model override
```
```yaml
# Executor (full tool access)
id: move-dev
name: "Move Developer"
role: doer
agent_class: executor
description: "Writes Move smart contracts"
vault_path: .quorum/vaults/move-dev/
context_file: .quorum/vaults/move-dev/CONTEXT.md
skill_file: ~/.claude/skills/quorum-roles/doer/SKILL.md     # auto-detected from role
model: opus                     # optional: per-agent model override
executor:
  target_dir: ~/nonvis/my-project   # supports ~/ expansion
```

### Templates System

**Agent identity** (CONTEXT.md) and **behavioral skills** (SKILL.md) are separate concerns:
- `templates/agents/{role}.md` — CONTEXT.md templates with `{agent_name}`, `{description}`, `{target_dir}`, `{skill_name}` placeholders. Substituted by `agent_create.h` in `--no-ai` mode.
- `templates/skills/quorum-roles/{role}/SKILL.md` — behavioral patterns per role. Auto-detected by `agent_create.h` from `~/.claude/skills/quorum-roles/{role}/SKILL.md`.
- `templates/skills/sui-dev-skills/` and `templates/skills/move-code-quality/` — domain expertise skills. Installed via `scripts/install-skills.sh`.

**Skill auto-detection:** When `agent create` runs without `--skill-file`, it checks `~/.claude/skills/quorum-roles/{role}/SKILL.md`. If found, it's automatically set in the agent YAML.

**Universal Rules:** All agent templates include a `## Universal Rules` section that enforces HANDOFF discipline: never self-HANDOFF, always include SUMMARY before HANDOFF, preserve task number prefixes ("Task N:") through the chain, and role-specific routing (doers/reviewers -> scribe, scribe -> done, leader -> architect/thinker).

### Project-Local Layout (`.quorum/`)

`quorum init` creates a self-contained `.quorum/` directory with a ready-to-use SQLite database:
```
myproject/.quorum/
  config.yaml, .gitignore, quorum.db (schema pre-created), quorum.pid
  agents/     — agent YAML configs (auto-discovered)
  vaults/     — per-agent vaults (CONTEXT.md + knowledge/)
  teams/      — team presets (named default_path configs)
```

**Schema management:** `storage/schema.h` defines `create_schema()` — all CREATE TABLE/INDEX statements in one place. Called by both `quorum init` (so DB is usable immediately) and `init_schema()` in main.cpp (which additionally runs ALTER TABLE migrations for old databases).

**Team presets** (`teams/*.yaml`): `name` + `default_path` array. Selected via `--team <name>`. Overrides `conversations.default_path` for that conversation. All agents remain available for HANDOFF regardless of team.

**Auto-discovery:** Without `--config`, the daemon walks up from cwd for `.quorum/config.yaml`, then `chdir`s to project root. All CLI commands work without flags from anywhere inside a `.quorum/` project.

**Config loading:** If `agents/` dir exists next to config, `load_agents_from_directory()` scans for `.yaml`/`.yml` files sorted by id, replacing any explicit `agents:` list.

**Web UI stale detection:** `GET /api/daemon/status` returns `{ running: boolean }` (checks PID file + `process.kill(pid, 0)`, resolves relative PID paths against project root). When daemon is not running and active conversations exist, the UI shows an amber warning banner advising the user to run `quorum status` to trigger crash recovery.

**Web UI converse behavior:** `POST /api/converse` checks `isDaemonRunning()` first. If daemon is running, uses `execDaemon()` (CLI inserts conversation, exits immediately). If not running, uses `spawnDaemon()` (starts daemon in background).

**Web UI management features:**
- **Project init:** `POST /api/init` runs `quorum_daemon init` via `execDaemonAt(cwd)` (no `--config` flag). UI prompts "Initialize Quorum" when selecting a path without `.quorum/`.
- **Agent creation:** `POST /api/agents` runs `quorum_daemon agent create --no-ai`. Form with role pills, name, description, optional target-dir and skill file (doer only).
- **Team builder:** `POST /api/teams`, `PUT /api/teams/:id`, `DELETE /api/teams/:id` — direct YAML file operations (no daemon needed). Drag-to-build agent path UI.
- **CONTEXT.md editor:** `GET/PUT /api/agents/:id/context` — read/write vault CONTEXT.md. Click any agent badge to open a modal markdown editor. Creates vault dir on first save.

## What NOT To Do

1. **Never add LLM calls to the daemon** — the orchestrator is deterministic
2. **Never use nlohmann/json or any JSON library** — manual json.h only
3. **Never use Boost**
4. **Never block the daemon event loop**
5. **Never run `claude -p` without budget enforcement**
6. **Never let analyst agents write files** — enforced via `--disallowedTools`

## Known Issues

- **Nested Claude Code sessions:** `claude -p` refuses to launch inside another Claude Code session (`CLAUDECODE` env var detected).
- **Buffered stdout in background mode:** When stdout is redirected, `std::cout` uses full buffering. Add `std::flush` for real-time log tailing.

## Useful Commands

```bash
# Build
cmake -B build && cmake --build build -j$(nproc)

# Tests
cd build && ctest --output-on-failure

# Initialize project
cd ~/myproject && quorum init

# Agent management (auto-discovers .quorum/)
quorum agent create --role doer --name my-dev --target-dir .              # auto-detects role skill
quorum agent create --role doer --name my-dev --target-dir . --no-ai     # uses templates/agents/doer.md
quorum agent create --role doer --name my-dev --target-dir . --skill move-developer
quorum agent modify --name my-dev --role thinker
quorum agent modify --name my-dev --skill move-developer
quorum agent list
quorum skills
quorum teams

# Template management
./scripts/lint-templates.sh                    # validate all templates
./scripts/install-skills.sh                    # copy skills to ~/.claude/skills/
./scripts/install-skills.sh --link             # symlink instead of copy
./scripts/update-templates.sh                  # review all role skills via claude -p
./scripts/update-templates.sh leader           # review one role skill

# Run daemon
./build/quorum_daemon                                          # auto-discover .quorum/
./build/quorum_daemon --config /path/to/config.yaml --verbose  # explicit config (escape hatch)

# Conversations
quorum converse "Build a REST API"
quorum converse --team quick-build "fix the login bug"
quorum status
quorum resume --conversation 1
quorum close --conversation 1

# What the daemon spawns (for reference)
claude -p "prompt" --dangerously-skip-permissions --disallowedTools "Write,Edit,NotebookEdit" --output-format json  # analyst
cd ~/project && claude -p "prompt" --dangerously-skip-permissions --output-format json                              # executor
```
