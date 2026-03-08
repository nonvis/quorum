# Quorum — Claude Code Instructions

## Project Overview

Quorum is a **multi-agent orchestration framework**. A deterministic C++20 daemon orchestrates independent AI agents that coordinate through structured proposals and persist knowledge in local vaults.

**Current phase: Phase 0 — Pure local orchestration.** No blockchain, no Walrus, no Seal. Everything runs on a single MacBook. The daemon spawns `claude -p` (Claude Code CLI in non-interactive mode) as the agent runtime. Web3 layers (Sui, Walrus, Seal) will be added in later phases after the core orchestration loop is proven.

**Tagline:** "Your AI agents now have verifiable memory and auditable decisions."

## Repo Layout

```
quorum/
├── CLAUDE.md                    ← You are here
├── quorum-core/                 # C++20 (CLOSED SOURCE) — orchestrator daemon
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── main.cpp             # Daemon entry, signal handling, PID lock, CLI subcommands
│   │   ├── daemon/              # Scheduler, router, message bus, events, conversation engine
│   │   ├── agent/               # Claude Code invoker (claude -p), context assembler, output parser
│   │   ├── knowledge/           # InboxWriter — writes OBSERVATION blocks to knowledge inbox
│   │   ├── vault/               # Local vault manager (filesystem-based)
│   │   ├── chain/               # [DEFERRED] Sui RPC client, proposals, audit, PTB
│   │   ├── seal/                # [DEFERRED] Seal encrypt/decrypt, access policies
│   │   ├── storage/             # SQLite (WAL mode) — task queue, token tracking, conversations
│   │   ├── utils/               # HTTP (libcurl), JSON (manual), crypto (ed25519), config, UUID
│   │   ├── sdk/                 # [DEFERRED] libquorum public API
│   │   └── cli/                 # quorum binary CLI commands
│   ├── tests/
│   └── configs/                 # Agent YAML definitions, task YAML definitions
│
├── data/                        # Runtime data (not compiled)
│   ├── vaults/                  # Per-agent vaults (CONTEXT.md, knowledge/, inbox/)
│   └── knowledge/               # Shared knowledge base (inbox/, library/, archive/)
│       └── PROCESSING.md        # Instructions for knowledge synthesis agent
├── scripts/                     # Shell scripts (smoke tests, utilities)
├── quorum-contracts/            # [DEFERRED] Move (OPEN SOURCE) — on-chain state machines
├── quorum-ts/                   # [DEFERRED] TypeScript (OPEN SOURCE) — community-facing
└── quorum-docs/                 # Documentation (OPEN SOURCE)
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
./build/quorum_daemon --config configs/quorum.yaml

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

The orchestrator daemon (`quorum_daemon`) **never** calls an LLM. All scheduling, routing, consensus, and event handling is pure C++ logic. Agent invocations happen by spawning `claude -p` subprocesses (`src/agent/invoker.h`), triggered by the daemon's task dispatch loop.

### Agent Invocation (Phase 0)

The daemon spawns Claude Code CLI as the agent runtime:

```
daemon → assembles prompt (vault + task) → spawns `claude -p "..." --dangerously-skip-permissions --disallowedTools "Write,Edit,NotebookEdit" [--session-id <uuid> | -r <uuid>] --output-format json` → collects stdout → parses result → writes to vault → routes follow-up tasks
```

**Session management:** The Invoker (`src/agent/invoker.h`) reads `session_id` from the `tasks` table. If a session_id is present:
- **First use** (no prior completed task with that session_id) → `--session-id <uuid>` (creates a new named session)
- **Subsequent use** (a completed task already used that session_id) → `-r <uuid>` (resumes the existing session)
- **No session_id** (Task Queue mode) → no flag (fresh anonymous session, current behavior)

**Resume fallback:** If `-r` fails (non-zero exit), the Invoker automatically retries with `--session-id` (fresh session). This handles expired/corrupted sessions gracefully.

**Session ID source:** `InvocationResult` includes the `session_id` returned by `claude -p` JSON output (or the task's session_id as fallback). The `ConversationEngine` generates session IDs via `generate_uuid()` from `utils/uuid.h`.

Task Queue mode tasks (no `conversation_id`) have NULL session_id and behave identically to pre-session behavior — each `claude -p` call is a fresh context. The vault provides continuity for these tasks.

### Agent Output Rules (Defense-in-Depth)

Agents must produce structured blocks (VAULT_UPDATE, OBSERVATION, PROPOSAL, SUMMARY) in their response text — never write files directly. This is enforced at three layers:

1. **`--disallowedTools`** — the invoker passes `--disallowedTools "Write,Edit,NotebookEdit"` to `claude -p`, removing file-writing tools entirely from the agent's tool list. This is the hard enforcement layer — agents cannot write files even if they try. Agents retain Read, Bash, Grep, Glob.
2. **CONTEXT.md** — each agent's vault `CONTEXT.md` has a "## CRITICAL — Output Rules" section (between Role and Core Question) prohibiting file writes and requiring structured blocks.
3. **Context assembler** — `src/agent/context_assembler.h` injects the same rules into every assembled prompt as a failsafe, between the "Current Task" and "Output Instructions" sections.

Template for new agents: `docs/CONTEXT_TEMPLATE.md`.

### Sequential Dispatch

The daemon enforces strictly sequential execution — one `claude -p` task at a time, always. This is a design decision, not a configuration:

- **One task at a time** — dispatch blocks while any task is active (`active > 0`)
- **No `max_concurrent` setting** — removed from `BudgetConfig` and `quorum.yaml`
- **Per-task token cap** — kills process if exceeded
- **Global daily/hourly budget** — daemon pauses all invocations when hit
- **Rationale:** causal traceability (task B depends on task A's vault updates), budget predictability, session resume chaining, deterministic debugging
- Critical for unattended overnight runs

### Proposal State Machine

```
DRAFT(0) → REVIEWING(1) → APPROVED(2) → EXECUTED(5) → EVALUATED(6)
                         → REJECTED(3)
                         → ESCALATED(4)
```

- Max 3 rounds per proposal
- Required reviewers declared at creation
- All transitions tracked locally in SQLite (Phase 0)
- On-chain transitions via PTB deferred to Phase 1+

### Vault System

Each agent owns a vault: `data/vaults/{agent_name}/`

```
data/vaults/{agent_name}/
├── CONTEXT.md          # Always loaded — role description + instructions
├── knowledge/          # Accumulated analysis and conclusions
├── experiments/        # Experiment designs and results
├── decisions/          # Past decisions linked to proposals
└── inbox/              # Items from other agents via proposal/consensus
```

- Local filesystem only (Phase 0) — no Walrus sync
- SQLite index for fast lookups
- Cross-agent reads through proposal review process (local enforcement)

### Knowledge System

Agents produce OBSERVATION blocks (timestamped, accumulated, never overwritten) in addition to VAULT_UPDATE blocks (distilled beliefs, overwritten). Observations flow through a 3-zone knowledge pipeline:

```
data/knowledge/
├── PROCESSING.md       # Instructions for the knowledge synthesis agent
├── inbox/              # Raw observations from agents (timestamped markdown files)
├── library/            # Synthesized findings by topic (findings.md per topic)
└── archive/            # Superseded topics (>30 days old, all evidence outdated)
```

- **InboxWriter** (`src/knowledge/inbox_writer.h`): daemon writes each OBSERVATION to `inbox/` as `{date}_{time}_{agent}_{task_type}.md` with YAML frontmatter (`agent`, `task_type`, `date`, `tags`, `processed: false`)
- **Synthesis**: a future knowledge-processing task reads `PROCESSING.md`, merges unprocessed inbox notes into `library/{topic}/findings.md`, and marks notes as `processed: true`
- Agents see both VAULT_UPDATE and OBSERVATION in their output instructions (context assembler includes both block formats)

### Conversation Mode (Phase 0.7)

Conversation Mode seeds a single goal and lets the daemon drive a Thinker/Reviewer pipeline through a state machine. The `ConversationEngine` (`src/daemon/conversation.h`) manages state transitions, task creation, and budget enforcement per conversation.

**State machine:**

```
INIT → THINKING → REVIEWING ──┬──→ APPROVED → DONE (Phase 0.7: no executor)
                      │        │
                 REVISE (round < max_rounds)
                      │        │
                      ▼        │
                   THINKING ◄──┘
                      │
                 REJECT or round >= max_rounds
                      │
                      ▼
                    CLOSED

Any state → PAUSED (budget exceeded, token anomaly, consecutive failures, or agent escalation)
```

**Key types:**
- `ConversationRecord` — struct in `storage/database.h` (id, goal, state, round, max_rounds, budget_usd, spent_usd)
- `ConversationEngine` — header-only class in `daemon/conversation.h`. Depends only on `Database` and `OutputParser` types. No dependency on Invoker, VaultManager, or ConsensusEngine.
- `PauseCheck` — struct in `daemon/conversation.h` (should_pause, reason). Returned by `check_pause_conditions()`.

**Database schema:**
- `conversations` table: id, goal, state, round, max_rounds, budget_usd, spent_usd, created_at, completed_at, paused_reason
- `tasks` table extended with: `conversation_id INTEGER REFERENCES conversations(id)`, `session_id TEXT`

**Session ID strategy (implemented in Invoker):**
- Thinker: same session_id reused across all REVISE cycles — Invoker detects prior completed task with same session_id and uses `-r` to resume
- Reviewer: fresh session_id per review task — Invoker uses `--session-id` (first use, no prior completion)
- Task Queue tasks: no session_id (NULL) — Invoker adds no session flag (backward compatible)
- UUID generation: centralized `generate_uuid()` in `utils/uuid.h` (UUID v4, `snprintf`-based, `mt19937_64`)

**Pause conditions (`check_pause_conditions()`):**
Centralized pause check runs in `on_task_complete()` before any state transition. Four triggers:
1. **Budget exceeded** — `spent_usd >= budget_usd` after cost update
2. **Token anomaly** — current task's `token_in > 2x median` of all completed tasks in the conversation (skipped if no history, i.e. median == 0)
3. **Consecutive failures** — last 2 tasks (by id DESC) for this conversation both have `status='failed'`
4. **Agent escalation** — reviewer produces `verdict: escalate` (caught both in pre-routing check and in `handle_reviewing()` verdict dispatch)

Helper methods: `get_median_input_tokens()`, `count_recent_failures()`, `get_task_token_in()` — all query the tasks table directly.

**Verdict normalization (`OutputParser::normalize_verdict()`):**
The parser normalizes REVIEW verdicts at parse time: lowercase + trim + alias mapping. Canonical values: `approve`, `reject`, `revise`, `escalate`. Unknown verdicts default to `reject` (safe). Aliases include `approved`/`accepted` -> `approve`, `rejected`/`denied` -> `reject`, `revision`/`needs_revision` -> `revise`, `escalated`/`needs_human`/`uncertain` -> `escalate`. Defense-in-depth: `handle_reviewing()` also lowercases before matching.

**Main loop integration:**
- After existing output processing (vault updates, consensus, observations), the dispatch lambda checks `get_conversation_for_task(task_id)`. If the task belongs to a conversation, `conversation_engine.on_task_complete()` routes to the next state. Tasks without `conversation_id` skip this block entirely (backward compatible).

### Storage (Phase 0)

```
Local only:  SQLite task queue, token tracking, vault index, proposal state, conversations
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
# configs/quorum.yaml — daemon config
daemon:
  pid_file: /tmp/quorum.pid
  data_dir: ./data
  log_level: info

chain:
  network: testnet          # testnet | mainnet
  rpc_url: https://fullnode.testnet.sui.io:443
  package_id: "0x..."

walrus:
  aggregator_url: https://aggregator.walrus-testnet.walrus.space
  publisher_url: https://publisher.walrus-testnet.walrus.space

agents:
  - config: configs/agents/market_analyst.yaml
  - config: configs/agents/bot_analyst.yaml
  - config: configs/agents/engineer.yaml
  - config: configs/agents/operator.yaml
```

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
4. **Never allow direct vault writes across agents** — cross-agent access goes through proposal review
5. **Never skip the proposal protocol** — all material decisions go through create→review→approve→execute→evaluate
6. **Never block the daemon event loop** — subprocess spawning must be non-blocking
7. **Never run `claude -p` without token budget enforcement** — per-task cap + global daily cap
8. **Never let agents write files directly** — enforced via `--disallowedTools "Write,Edit,NotebookEdit"`; all output must be structured blocks in the response text; the daemon parses and routes them

## Current Phase

**Phase 0.7: Conversation Mode** — Pure local orchestration with goal-driven Thinker/Reviewer pipeline.

Priority order:
1. ~~C++ daemon skeleton~~ ✓ (main.cpp, signal handling, PID lock, config loading)
2. ~~Scheduler, message bus, router, event dispatcher~~ ✓ (skeleton implementations)
3. ~~Invoker rewrite~~ ✓ (spawns `claude -p`, captures JSON output, writes token/cost to DB)
4. ~~SQLite task queue~~ ✓ (pending/active/done states with token tracking)
5. ~~Context assembler~~ ✓ (vault CONTEXT.md + knowledge + inbox, output format instructions incl. OBSERVATION guidance)
6. ~~Output parser~~ ✓ (VAULT_UPDATE / PROPOSAL / REVIEW / OBSERVATION / SUMMARY blocks, KV + multi-line parsing, lenient block detection for heading/bold labels and first-line type fallback, language-tagged fences, content fallback for free-text blocks, field aliases for reviewer/verdict variants, VAULT_UPDATE path aliases `target:`/`file:` and bold metadata path extraction, verdict normalization with alias mapping)
7. ~~Token budget enforcement~~ ✓ (per-task cap + hourly/daily caps with rolling window)
8. ~~Smoke test script~~ ✓ (scripts/smoke_test.sh — seeds tasks, runs daemon, validates results; includes WAL/SHM cleanup to prevent stale SQLite state)
9. ~~End-to-end dispatch verified~~ ✓ (daemon claims pending tasks, invokes `claude -p`, writes results back to DB)
10. ~~Knowledge pipeline wired~~ ✓ (OBSERVATION blocks parsed → InboxWriter → `data/knowledge/inbox/`; PROCESSING.md for synthesis agent; all 4 agent CONTEXT.md files updated with OBSERVATION block format)
11. ~~Conversation schema + CRUD~~ ✓ (ConversationRecord struct, 8 Database CRUD methods, conversations table, tasks extended with conversation_id + session_id, ALTER TABLE migration for existing DBs)
12. ~~ConversationEngine~~ ✓ (header-only state machine in daemon/conversation.h — start, on_task_complete, resume, close; Thinker/Reviewer pipeline with revise loops, multi-trigger pause system, escalation protocol, session_id reuse)
13. ~~Main loop integration~~ ✓ (conversation routing block in task_dispatch lambda — runs after existing output processing, backward compatible for non-conversation tasks)
14. ~~Session resume in Invoker~~ ✓ (reads session_id from tasks table, uses `--session-id` for first use / `-r` for resume; fallback retry on resume failure; `generate_uuid()` centralized in `utils/uuid.h`; `InvocationResult` includes session_id; verbose log shows session prefix)
15. ~~Pause conditions + escalation~~ ✓ (centralized `check_pause_conditions()` with 4 triggers: budget exceeded, token anomaly >2x median, 2+ consecutive failures, agent escalation verdict; `PauseCheck` struct; `normalize_verdict()` in OutputParser maps aliases to canonical values; "escalate" verdict in handle_reviewing; agent CONTEXT.md files updated with REVIEW Verdicts section)
16. ~~Sequential dispatch enforcement~~ ✓ (removed `max_concurrent` from BudgetConfig/config parser/quorum.yaml; dispatch gate changed from `active >= max_concurrent` to `active > 0`; design decision, not configuration; test_pipeline updated from parallelism gate to sequential dispatch test)
17. ~~CLI subcommands~~ ✓ (`converse`, `status`, `resume`, `close` subcommands in main.cpp — two-phase arg parser, early-exit for status/close without PID lock, graceful PID lock fallback for converse/resume when daemon already running, `print_conversations()` for status display)
18. ~~Conversation pipeline integration test~~ ✓ (`tests/integration/test_conversation_pipeline.cpp` — 9 tests, 34 assertions, exercises full state machine end-to-end without `claude -p`: happy path, revise with session reuse, max rounds exhaustion, budget pause, consecutive failures pause, agent escalation pause, resume from paused, operator close, reject close; all 11 ctest targets pass)

**Goal:** Daemon spawns `claude -p` processes, manages task queue, coordinates multiple agents through filesystem vaults. Fully automated, runs unattended for hours.

### Known Issues

- **Nested Claude Code sessions:** `claude -p` refuses to launch inside another Claude Code session (`CLAUDECODE` env var detected). The smoke test must be run from a regular terminal, not from within `claude` CLI.
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
./build/quorum_daemon --config configs/quorum.yaml

# Run with verbose logging
./build/quorum_daemon --config configs/quorum.yaml --verbose

# Start a conversation + daemon
./build/quorum_daemon --config configs/quorum.yaml converse "Analyze market trends"

# Start with custom budget and max rounds
./build/quorum_daemon --config configs/quorum.yaml converse --budget 3.0 --max-rounds 5 "goal text"

# List all conversations (no PID lock, no daemon)
./build/quorum_daemon --config configs/quorum.yaml status

# Resume a paused conversation (works with or without running daemon)
./build/quorum_daemon --config configs/quorum.yaml resume --conversation 1

# Close a conversation (no PID lock, no daemon)
./build/quorum_daemon --config configs/quorum.yaml close --conversation 1

# Agent invocation (what the daemon spawns)
claude -p "prompt" --dangerously-skip-permissions --disallowedTools "Write,Edit,NotebookEdit" --output-format json

# Agent invocation with session (conversation mode)
claude -p "prompt" --dangerously-skip-permissions --disallowedTools "Write,Edit,NotebookEdit" --session-id <uuid> --output-format json  # first use
claude -p "prompt" --dangerously-skip-permissions --disallowedTools "Write,Edit,NotebookEdit" -r <uuid> --output-format json            # resume

# Smoke test (seeds tasks, runs daemon with real claude -p, validates results)
# MUST be run from a regular terminal, NOT inside a claude session
./scripts/smoke_test.sh

# Troubleshooting: clear stale SQLite WAL/SHM if daemon sees wrong data
rm -f data/quorum.db-wal data/quorum.db-shm
```
