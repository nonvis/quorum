# Quorum — Claude Code Instructions

## Project Overview

Quorum is a **multi-agent orchestration framework**. A deterministic C++20 daemon orchestrates independent AI agents that coordinate through structured HANDOFF blocks and persist knowledge in local vaults.

**Current phase: Phase 2 — Team Mode.** Pure local orchestration on a single MacBook. The daemon spawns `claude -p` (Claude Code CLI in non-interactive mode) as the agent runtime. Web3 layers (Sui, Walrus, Seal) are deferred indefinitely.

**Tagline:** "Define your agents, point them at your project, let the daemon run."

## Repo Layout

```
quorum/
├── CLAUDE.md                    ← You are here
├── configs/                     # Project configs (one YAML per project)
│   ├── mm-bot.yaml              # mm-bot project config (agents, budget, conversations)
│   ├── hello-world.yaml         # hello-world verification project config
│   ├── agents/
│   │   ├── mm-bot/              # mm-bot agent YAMLs (market_analyst, bot_analyst, etc.)
│   │   └── hello-world/         # hello-world agent YAMLs (thinker, executor, reviewer)
│   └── tasks/                   # Task YAML definitions
├── quorum-core/                 # C++20 (CLOSED SOURCE) — orchestrator daemon
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── main.cpp             # Daemon entry, signal handling, PID lock, CLI subcommands
│   │   ├── daemon/              # Scheduler, message bus, events, conversation engine
│   │   ├── agent/               # Claude Code invoker (claude -p), context assembler, output parser
│   │   ├── vault/               # Local vault manager (filesystem-based)
│   │   ├── chain/               # [DEFERRED] Sui RPC client, proposals, audit, PTB
│   │   ├── seal/                # [DEFERRED] Seal encrypt/decrypt, access policies
│   │   ├── storage/             # SQLite (WAL mode) — task queue, token tracking, conversations, knowledge ledger
│   │   ├── utils/               # HTTP (libcurl), JSON (manual), crypto (ed25519), config, UUID
│   │   ├── sdk/                 # [DEFERRED] libquorum public API
│   │   └── cli/                 # quorum binary CLI commands
│   └── tests/
├── data/                        # Runtime data (gitignored)
│   └── vaults/                  # Per-agent vaults (CONTEXT.md, knowledge/)
├── quorum-web/                  # Web dashboard (API + React frontend)
│   ├── config.ts                # Paths resolved relative to repo root
│   ├── server/
│   │   ├── index.ts             # Hono routes (REST + SSE)
│   │   ├── db.ts                # Read-only SQLite via bun:sqlite
│   │   ├── daemon.ts            # CLI wrapper (Bun.spawn → quorum_daemon)
│   │   └── sse.ts               # SSE stream + auto-approve poller
│   └── client/                  # React + Tailwind (Vite, port 3101)
│       └── src/
│           ├── App.tsx           # Root layout (SSE + refresh)
│           ├── api.ts            # Fetch wrappers for all endpoints
│           ├── types.ts          # Conversation, Task, Stats interfaces
│           ├── hooks/useSSE.ts   # EventSource hook for real-time updates
│           └── components/       # StatsBanner, PromptInput, ConversationCard, etc.
├── .claude/commands/             # Claude Code skills (project scaffolding, ops)
├── scripts/                     # Shell scripts (utilities)
└── docs/                        # Design documents
```

## Reference Codebase

The bot-manager project at `../bot-manager` is the primary C++ reference. Reuse its patterns for HTTP, JSON, crypto, SQLite, and general C++ style. When in doubt about how to implement something, check bot-manager first.

## Hard Constraints

### C++ Conventions (from bot-manager)

- **C++20 standard** — concepts, ranges, designated initializers, std::format
- **Namespace:** `sui::quorum`
- **No nlohmann/json** — use manual `json.h` parsing (copy pattern from bot-manager)
- **No Boost** — ever
- **No exceptions for control flow** — use return codes / std::optional / std::expected
- **Header-only preferred** for new modules (`.h` files with inline implementations)
- **Timestamps:** epoch seconds as `uint64_t` everywhere
- **Logging:** stdout/stderr only (match bot-manager; no spdlog unless explicitly decided)
- **String handling:** `std::string_view` for read-only, `std::string` for ownership
- **Error handling pattern:** return `bool` or `std::optional<T>` — never throw

### Dependencies (Phase 0)

| Dependency | Purpose | Source |
|-----------|---------|--------|
| sqlite3 | Task queue, token tracking, audit cache | Reuse from bot-manager |
| OpenSSL | Hashing (SHA-256), TLS | Reuse from bot-manager |
| libcurl | HTTP client (future use) | Reuse from bot-manager |

**Runtime dependency:** `claude` CLI (Claude Code) must be installed and authenticated.

**Banned:** nlohmann/json, Boost, any JSON library, any ORM, any framework

### Build System

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j$(nproc)

# Run daemon
./build/quorum_daemon --config configs/mm-bot.yaml

# Run tests
cd build && ctest --output-on-failure
```

### File Naming

- Headers: `snake_case.h` (e.g., `vault_manager.h`, `sui_client.h`)
- Sources: `snake_case.cpp` (only when not header-only)
- Tests: `test_<module>.cpp` (e.g., `test_scheduler.cpp`)
- Configs: `snake_case.yaml`
- Move: `snake_case.move`

## Architecture Rules

### The Daemon Is Deterministic

The orchestrator daemon (`quorum_daemon`) **never** calls an LLM. All scheduling, routing, and event handling is pure C++ logic. Agent invocations happen by spawning `claude -p` subprocesses (`src/agent/invoker.h`), triggered by the daemon's task dispatch loop.

### Agent Invocation (Phase 0)

The daemon spawns Claude Code CLI as the agent runtime:

```
daemon → assembles prompt (vault + task) → looks up AgentMetadata → spawns `claude -p` with class-appropriate flags → collects stdout → parses result → writes to vault → routes follow-up tasks
```

**Agent classes and tool policy:** The Invoker (`src/agent/invoker.h`) receives `AgentMetadata` (from `utils/config.h`) and conditionally builds the `claude -p` command:
- **`analyst`** (default): `--disallowedTools "Write,Edit,NotebookEdit"` — read-only tools only (Read, Bash, Grep, Glob)
- **`executor`**: no `--disallowedTools` flag — full tool access including Write, Edit, NotebookEdit
- If `AgentMetadata::target_dir` is set, the command is prefixed with `cd <target_dir> &&` (with `~/` expansion via `$HOME`). This sets the working directory for executor agents.
- Unknown or empty `agent_class` defaults to `"analyst"` (safe default via struct default initializer).

**Session management:** The Invoker reads `session_id` from the `tasks` table. If a session_id is present:
- **First use** (no prior completed task with that session_id) → `--session-id <uuid>` (creates a new named session)
- **Subsequent use** (a completed task already used that session_id) → `-r <uuid>` (resumes the existing session)
- **No session_id** (Task Queue mode) → no flag (fresh anonymous session, current behavior)

**Resume fallback:** If `-r` fails (non-zero exit), the Invoker automatically retries with `--session-id` (fresh session). This handles expired/corrupted sessions gracefully.

**Session ID source:** `InvocationResult` includes the `session_id` returned by `claude -p` JSON output (or the task's session_id as fallback). The `ConversationEngine` generates session IDs via `generate_uuid()` from `utils/uuid.h`.

Task Queue mode tasks (no `conversation_id`) have NULL session_id and behave identically to pre-session behavior — each `claude -p` call is a fresh context. The vault provides continuity for these tasks.

### Agent Output Rules (Defense-in-Depth)

Agents must produce structured blocks (VAULT_UPDATE, SUMMARY, HANDOFF, KNOWLEDGE) in their response text — never write files directly. This is enforced at three layers:

1. **`--disallowedTools`** — for `analyst` agents, the invoker passes `--disallowedTools "Write,Edit,NotebookEdit"` to `claude -p`, removing file-writing tools entirely from the agent's tool list. This is the hard enforcement layer — analyst agents cannot write files even if they try. **Exception:** `executor` agents (Phase 0.9) get full tool access and are expected to write files directly in their `target_dir`.
2. **CONTEXT.md** — each agent's vault `CONTEXT.md` has a "## CRITICAL — Output Rules" section (between Role and Core Question) prohibiting file writes and requiring structured blocks.
3. **Context assembler** — `src/agent/context_assembler.h` injects the same rules into every assembled prompt as a failsafe, between the "Current Task" and "Output Instructions" sections.

Template for new agents: `docs/CONTEXT_TEMPLATE.md`.

### Sequential Dispatch

The daemon enforces strictly sequential execution — one `claude -p` task at a time, always. This is a design decision, not a configuration:

- **One task at a time** — dispatch blocks while any task is active (`active > 0`)
- **No `max_concurrent` setting** — removed from `BudgetConfig` and `mm-bot.yaml`
- **Per-task token cap** — kills process if exceeded
- **Global hourly budget** — daemon pauses all invocations when hit
- **Rationale:** causal traceability (task B depends on task A's vault updates), budget predictability, session resume chaining, deterministic debugging
- Critical for unattended overnight runs

### Vault System

Each agent owns a vault: `data/vaults/{agent_name}/`

```
data/vaults/{agent_name}/
├── CONTEXT.md          # Always loaded — role description + instructions
└── knowledge/          # Accumulated analysis and conclusions
```

- Local filesystem only — no Walrus sync
- CONTEXT.md is the agent's identity and instructions
- knowledge/ holds accumulated findings via VAULT_UPDATE blocks

### Knowledge System

Agents produce KNOWLEDGE blocks during their turns. These are stored in the `knowledge_ledger` SQLite table (not the filesystem). The scribe agent consumes the ledger at the end of a conversation cycle to produce summaries.

**Knowledge ledger schema** (in `storage/database.h`):
- `knowledge_ledger` table: `id`, `cycle_id` (FK to conversations), `agent_id`, `turn_number`, `topic`, `content`, `created_at`
- Methods: `append_knowledge()`, `get_cycle_knowledge()`, `count_cycle_knowledge()`

Agents also produce VAULT_UPDATE blocks for persistent findings written to their own `knowledge/` directory. KNOWLEDGE blocks are ephemeral (per-cycle); VAULT_UPDATE blocks are persistent (per-agent vault).

### Conversation Mode (Phase 2 — Team Mode)

Conversation Mode seeds a single goal and lets the daemon coordinate a team of agents. The `ConversationEngine` (`src/daemon/conversation.h`) manages state transitions and budget enforcement per conversation.

**NOTE:** The legacy analyst/executor pipeline state machines were stripped in Phase 2 task #0. HANDOFF block parsing was added in task #1. KNOWLEDGE block parsing and `knowledge_ledger` table were added in task #2. The ConversationEngine is currently stubbed — `on_task_complete()` is a no-op returning `false`. Team mode routing (using parsed HANDOFF blocks) and knowledge ledger integration will be wired in task #3.

**States:** `active`, `waiting_for_human`, `done`, `closed`, `paused` (enum `ConvState`).

**Key types:**
- `ConversationRecord` — struct in `storage/database.h` (id, goal, state, round, max_rounds, budget_usd, spent_usd)
- `ConversationEngine` — header-only class in `daemon/conversation.h`. Constructor takes `Database&` only. Methods: `start()`, `on_task_complete()` (stub), `resume()`, `close()`.

**Database schema:**
- `conversations` table: id, goal, state, round, max_rounds, budget_usd, spent_usd, created_at, completed_at, paused_reason
- `tasks` table extended with: `conversation_id INTEGER REFERENCES conversations(id)`, `session_id TEXT`
- `knowledge_ledger` table: id, cycle_id (FK→conversations), agent_id, turn_number, topic, content, created_at. Methods: `append_knowledge()`, `get_cycle_knowledge()`, `count_cycle_knowledge()` in `database.h`.

**Session ID strategy (implemented in Invoker):**
- Task Queue tasks: no session_id (NULL) — Invoker adds no session flag
- Conversation tasks: session_id set per task — Invoker uses `--session-id` for first use / `-r` for resume
- UUID generation: centralized `generate_uuid()` in `utils/uuid.h` (UUID v4, `snprintf`-based, `mt19937_64`)

**Main loop integration:**
- After output processing (vault updates), the dispatch lambda checks `get_conversation_for_task(task_id)`. If the task belongs to a conversation, `conversation_engine.on_task_complete()` is called (currently a no-op stub). Tasks without `conversation_id` skip this block entirely.

### Storage (Phase 0)

```
Local only:  SQLite task queue, token tracking, vault index, conversations, knowledge ledger
Future:      Walrus (vault blobs), Sui (proposals, audit), Seal (access control)
```

## Key Patterns

### HTTP Client Pattern (from bot-manager)

```cpp
// Use the libcurl wrapper from utils/http_client.h
// All HTTP calls go through this — Sui RPC, Walrus API, LLM APIs
auto response = sui::quorum::http::post(url, headers, body);
if (!response) {
    std::cerr << "HTTP error: " << response.error() << "\n";
    return std::nullopt;
}
```

### JSON Pattern (from bot-manager)

```cpp
// Manual JSON extraction — no libraries
// See utils/json.h — flat key extraction functions that distinguish keys from values
auto text  = sui::quorum::json::extract_string(json_str, "result");   // -> std::optional<std::string>
auto cost  = sui::quorum::json::extract_number(json_str, "total_cost_usd"); // -> double
auto toks  = sui::quorum::json::extract_int(json_str, "input_tokens");      // -> int64_t
auto flag  = sui::quorum::json::extract_bool(json_str, "is_error");         // -> bool
```

### SQLite Pattern (from bot-manager)

```cpp
// WAL mode, mutex-protected, RAII
sui::quorum::Database db("quorum.db");
db.execute("CREATE TABLE IF NOT EXISTS metrics (...)");
auto result = db.query("SELECT * FROM metrics WHERE agent = ?", agent_id);
```

### Config Pattern

```yaml
# configs/mm-bot.yaml — daemon config (key sections)
daemon:
  pid_file: /tmp/quorum.pid
  data_dir: ./data
  log_level: info

budget:
  daily_limit_usd: 10.0
  hourly_limit_usd: 3.0
  task_timeout_seconds: 300

conversations:
  enabled: true
  leader: leader                              # agent id that receives the goal first
  default_max_rounds: 20                      # repurposed as max_turns
  default_budget_usd: 5.0                     # per-conversation budget cap
  default_path: [leader, thinker, doer, scribe]  # optional default routing path

agents:
  - config: configs/agents/project/leader.yaml
  - config: configs/agents/project/thinker.yaml
  - config: configs/agents/project/doer.yaml
  - config: configs/agents/project/scribe.yaml
```

### Agent YAML Pattern

```yaml
# configs/agents/<agent>.yaml — agent config (analyst)
id: market_analyst
name: "Market Analyst"
role: thinker                    # archetype: leader, thinker, doer, reviewer, scribe, librarian
agent_class: analyst             # analyst = read-only tools, no Write/Edit/NotebookEdit
description: "Analyzes market structure and trading patterns"
vault_path: data/vaults/market_analyst/
context_file: data/vaults/market_analyst/CONTEXT.md
skill_file: data/vaults/market_analyst/SKILL.md   # optional — domain-specific instructions
```

```yaml
# configs/agents/<agent>.yaml — agent config (executor)
id: move-dev
name: "Move Developer"
role: doer                       # doer role maps to executor agent_class
agent_class: executor            # executor = full tool access (Write, Edit, NotebookEdit)
description: "Writes Move smart contracts"
vault_path: data/vaults/move-dev/
context_file: data/vaults/move-dev/CONTEXT.md
executor:
  target_dir: ~/nonvis/my-project   # working directory for claude -p (supports ~/ expansion)
```

**Agent classes:** `analyst` (read-only, default — `--disallowedTools` enforced), `executor` (full tool access, no `--disallowedTools`). Role determines class: doer = executor, all others = analyst.

**Config loading:** At startup, `load_config()` reads the project config YAML (e.g. `mm-bot.yaml`) agent list, then calls `load_agent_config()` for each entry. This parses the individual agent YAML into `AgentMetadata` structs (id, name, agent_class, config_path, vault_path, context_file, target_dir). The Invoker receives `AgentMetadata` at dispatch time to build the appropriate `claude -p` command.

## Move Contract Conventions (Deferred — Phase 1+)

- All proposal state transitions enforce the state machine (assert on invalid transitions)
- Use `clock::timestamp_ms(clock)` for all timestamps
- Capability pattern: `AgentCap` for agent auth, `HumanApprovalCap` for human gate
- Heavy content on Walrus (referenced by blob ID as `vector<u8>`), lightweight metadata on-chain
- Shared objects for proposals (multi-party interaction), owned objects for agent identities

## TypeScript Conventions (Deferred — Phase 5+)

- pnpm workspaces with turborepo
- @quorum/sdk connects to daemon via HTTP REST API
- @quorum/cli wraps SDK with commander.js
- @quorum/dashboard is React + Tailwind
- All open source

## What NOT To Do

1. **Never add LLM calls to the daemon control loop** — the orchestrator is deterministic
2. **Never use nlohmann/json or any JSON library** — manual json.h only
3. **Never use Boost** — for anything
4. **Never block the daemon event loop** — subprocess spawning must be non-blocking
5. **Never run `claude -p` without token budget enforcement** — per-task cap + global daily cap
6. **Never let analyst agents write files directly** — enforced via `--disallowedTools "Write,Edit,NotebookEdit"` for `agent_class: analyst`; all analyst output must be structured blocks in the response text; the daemon parses and routes them. Executor agents (`agent_class: executor`) are exempt and get full tool access.

## Current Phase

**Phase 2: Team Mode** — Replace legacy analyst/executor pipelines with flexible team-based agent coordination.

Phase 1 completed items (preserved for reference):
1. ~~C++ daemon skeleton~~ ✓
2. ~~Scheduler, message bus, event dispatcher~~ ✓
3. ~~Invoker rewrite~~ ✓ (spawns `claude -p`, captures JSON output, writes token/cost to DB)
4. ~~SQLite task queue~~ ✓ (pending/active/done states with token tracking)
5. ~~Context assembler~~ ✓
6. ~~Output parser~~ ✓ (HANDOFF / KNOWLEDGE / VAULT_UPDATE / SUMMARY blocks)
7. ~~Token budget enforcement~~ ✓ (per-task cap + hourly cap with rolling window)
8. ~~End-to-end dispatch verified~~ ✓
9. ~~Conversation schema + CRUD~~ ✓
10. ~~Session resume in Invoker~~ ✓
11. ~~Sequential dispatch enforcement~~ ✓
12. ~~CLI subcommands~~ ✓ (`converse`, `status`, `resume`, `close`)
13. ~~ConversationConfig~~ ✓ (enabled, default_max_rounds, default_budget_usd)
14. ~~AgentMetadata + executor support in Invoker~~ ✓
15. ~~Multi-project config layout~~ ✓

Phase 2 tasks:
0. ~~Strip legacy pipelines~~ ✓ (removed analyst/executor pipeline state machines from ConversationEngine, removed router.h, removed consensus/observations/proposals processing from main loop, removed gate CLI subcommand, removed pipeline field from ConversationRecord/DB schema, stubbed on_task_complete(), deleted 8 files incl. legacy tests/scripts, 9 tests pass)
1. ~~HANDOFF block parsing~~ ✓ (added HandoffBlock struct, std::optional<HandoffBlock> in ParsedOutput, registered HANDOFF in all 3 detection points + dispatch_block, content fallback for free-text prompts, last-HANDOFF-wins semantics, 9 test cases in test_handoff_parser.cpp, 10 tests pass)
2. ~~KNOWLEDGE block parsing + knowledge_ledger~~ ✓ (added KnowledgeBlock struct, std::vector<KnowledgeBlock> in ParsedOutput, registered KNOWLEDGE in all 3 detection points + dispatch_block, content fallback for free-text, push_back semantics, knowledge_ledger SQLite table with append/get/count methods in database.h, 8 parser tests + 3 ledger tests, 12 tests pass)
3. Generic conversation loop — rewrite ConversationEngine with team mode ball-passing via HANDOFF blocks
4. Team roster injection — inject agent roster into leader's context
5. Human interaction — respond CLI subcommand, waiting_for_human state
6. Config parser — parse leader, default_path, role, skill_file from YAML
7. State simplification — clean up ConvState transitions for team mode
8. Agent generator — scaffold new agent configs from archetypes
9. Integration tests — team mode end-to-end test suite
10. E2E verification — full conversation loop with real agents

**Goal:** Daemon spawns `claude -p` processes, manages task queue, coordinates multiple agents through filesystem vaults. Fully automated, runs unattended for hours.

### Known Issues

- **Nested Claude Code sessions:** `claude -p` refuses to launch inside another Claude Code session (`CLAUDECODE` env var detected).
- ~~**Invoker error handling:**~~ ✓ Fixed — invoker now checks exit code and validates JSON structure (`"type":"result"`) before calling `mark_done()`. Non-zero exits and invalid output are routed to `mark_failed()`. See `CommandResult` struct and `validate_claude_output()` in `src/agent/invoker.h`.
- **Buffered stdout in background mode:** When daemon stdout is redirected to a file, `std::cout` uses full buffering. Verbose log lines only appear after process exit. Add `std::flush` to verbose output paths if real-time log tailing is needed.
- ~~**Agents writing files directly:**~~ ✓ Fixed — Phase 0.5 observation test revealed all agents used Claude Code's built-in Write/Edit tools instead of structured blocks, bypassing the output parser entirely. Prompt-level instructions alone were insufficient — agents treat available tools as the preferred path. Definitive fix: `--disallowedTools "Write,Edit,NotebookEdit"` in the invoker removes the tools entirely. Defense-in-depth via CONTEXT.md and context_assembler.h prompt instructions retained as secondary layers.
- ~~**Zero blocks through pipeline:**~~ ✓ Fixed — Test 4 revealed agents produce structured blocks but 0 flow through the parser. Three root causes: (1) agents wrap blocks in language-tagged fences (`\`\`\`markdown`) which neither `detect_block_open` nor `is_plain_fence` matched — added `starts_with_fence()` for type-header + language-tagged fence combos; (2) agents write free-text content after `---` separator inside blocks, which `parse_kv()` silently drops (no colon = no key) — added `join_lines()` content fallback in `dispatch_block()` when `content` field is empty; (3) agents use `required_reviewers` / `reviewers` instead of `requires_consensus_from` — added field alias chains in PROPOSAL and REVIEW dispatch. 29 tests pass (19 existing + 10 new).
- ~~**VAULT_UPDATE empty paths:**~~ ✓ Fixed — Test 6 revealed 4 of 5 VAULT_UPDATE blocks rejected by vault_manager because `vu.path` was empty. Two causes: (A) agents write `target:` or `file:` instead of `path:` inside blocks — added field alias chain in `dispatch_block()` (path → target → file); (B) agents put path in bold metadata above the code fence (`**file:** \`knowledge/foo.md\``) which never reached `parse_kv()` — added `extract_metadata_path()` helper and `pending_path` state in `parse()` that injects a synthetic `path:` line on block entry. Explicit path inside block takes precedence (last-wins). 36 tests pass (29 existing + 7 new).

## Useful Commands

```bash
# Build
cmake -B build && cmake --build build -j$(nproc)

# Run daemon (no subcommand — plain daemon mode)
./build/quorum_daemon --config configs/mm-bot.yaml

# Run with verbose logging
./build/quorum_daemon --config configs/mm-bot.yaml --verbose

# Start a conversation + daemon
./build/quorum_daemon --config configs/mm-bot.yaml converse "Analyze market trends"

# Start with custom budget and max rounds
./build/quorum_daemon --config configs/mm-bot.yaml converse --budget 3.0 --max-rounds 5 "goal text"

# List all conversations (no PID lock, no daemon)
./build/quorum_daemon --config configs/mm-bot.yaml status

# Resume a paused conversation (works with or without running daemon)
./build/quorum_daemon --config configs/mm-bot.yaml resume --conversation 1

# Close a conversation (no PID lock, no daemon)
./build/quorum_daemon --config configs/mm-bot.yaml close --conversation 1

# Agent invocation — analyst (what the daemon spawns, read-only tools)
claude -p "prompt" --dangerously-skip-permissions --disallowedTools "Write,Edit,NotebookEdit" --output-format json

# Agent invocation — executor (full tool access, with target_dir)
cd ~/projects/myapp && claude -p "prompt" --dangerously-skip-permissions --output-format json

# Agent invocation with session (conversation mode)
claude -p "prompt" --dangerously-skip-permissions --disallowedTools "Write,Edit,NotebookEdit" --session-id <uuid> --output-format json  # first use
claude -p "prompt" --dangerously-skip-permissions --disallowedTools "Write,Edit,NotebookEdit" -r <uuid> --output-format json            # resume

# Troubleshooting: clear stale SQLite WAL/SHM if daemon sees wrong data
rm -f data/quorum.db-wal data/quorum.db-shm
```

## Claude Code Skills

Interactive skills in `.claude/commands/` — use `/skill-name` in Claude Code:

| Skill | Purpose |
|-------|---------|
| `/quorum-setup` | Scaffold a new project or initialize data/ for an existing config |

Skills replace shell scripts for project scaffolding. They can ask questions, read existing configs for patterns, validate what they create, and adapt — no rigid script to maintain.
