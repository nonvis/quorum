#!/usr/bin/env bash
# smoke_test.sh — Live smoke test for the Quorum daemon
#
# Seeds 2 tasks (market_analyst + bot_analyst), starts the daemon in verbose
# mode, and prints a validation summary after the user stops it (Ctrl+C).
#
# Run from the quorum/ root directory:
#   chmod +x scripts/smoke_test.sh
#   ./scripts/smoke_test.sh
#
# Estimated cost: $0.15-0.30 (2 seed tasks + auto-scheduled review tasks)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CORE_DIR="$ROOT_DIR/quorum-core"
DATA_DIR="$ROOT_DIR/data"
DB_PATH="$DATA_DIR/quorum.db"
CONFIG_PATH="$ROOT_DIR/configs/quorum.yaml"
DAEMON_BIN="$CORE_DIR/build/quorum_daemon"

# ─── Colors ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

info()  { echo -e "${CYAN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; }
ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }

# ─── 1. Clean slate ──────────────────────────────────────────────────────────
info "Cleaning previous state..."
if [ -f "$DB_PATH" ]; then
    rm "$DB_PATH"
    ok "Removed existing $DB_PATH"
else
    ok "No existing database — fresh start"
fi

# ─── 2. Build ─────────────────────────────────────────────────────────────────
info "Building quorum_daemon..."
mkdir -p "$CORE_DIR/build"
(cd "$CORE_DIR/build" && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)")
if [ ! -x "$DAEMON_BIN" ]; then
    error "Build failed — $DAEMON_BIN not found"
    exit 1
fi
ok "Build complete: $DAEMON_BIN"

# ─── 3. Seed tasks via SQLite ─────────────────────────────────────────────────
info "Seeding tasks into $DB_PATH..."
mkdir -p "$DATA_DIR"

# Read agent CONTEXT.md files
MARKET_CONTEXT="$(cat "$DATA_DIR/vaults/market_analyst/CONTEXT.md" 2>/dev/null || echo '(market_analyst CONTEXT.md not found)')"
BOT_CONTEXT="$(cat "$DATA_DIR/vaults/bot_analyst/CONTEXT.md" 2>/dev/null || echo '(bot_analyst CONTEXT.md not found)')"

# Output format instructions (matches context_assembler.h + REVIEW block)
OUTPUT_INSTRUCTIONS='---

# Output Instructions

When you have findings, use these structured blocks in your response:

```VAULT_UPDATE
path: knowledge/<filename>.md
content: |
  <content to write>
```

```PROPOSAL
title: <title>
requires_consensus_from: [<agent_names>]
content: |
  <proposal details>
```

```REVIEW
proposal_id: <proposal-id>
verdict: approve | reject | escalate
reasoning: |
  <multi-line reasoning>
```

```SUMMARY
<brief findings summary>
```'

# Build prompts
TASK1_PROMPT="# Agent Context

${MARKET_CONTEXT}

---

# Current Task

**Task type:** routine_scan
**Agent:** market_analyst

Review your context and current knowledge. Write a brief status update as a VAULT_UPDATE to knowledge/status.md. Then create a PROPOSAL to investigate SUI/USDC pool spread opportunities. Require consensus from bot_analyst.

${OUTPUT_INSTRUCTIONS}"

TASK2_PROMPT="# Agent Context

${BOT_CONTEXT}

---

# Current Task

**Task type:** routine_scan
**Agent:** bot_analyst

Review your context. Write a brief status update as a VAULT_UPDATE to knowledge/status.md summarizing your readiness to analyze bot performance.

${OUTPUT_INSTRUCTIONS}"

# Create tasks table and seed tasks
# (The daemon's init_schema will create the remaining tables on startup.)
sqlite3 "$DB_PATH" <<'SCHEMA'
CREATE TABLE IF NOT EXISTS tasks (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  agent TEXT NOT NULL,
  task_type TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'pending',
  prompt TEXT NOT NULL,
  result TEXT,
  token_in INTEGER,
  token_out INTEGER,
  cost REAL,
  error TEXT,
  created_at TEXT NOT NULL DEFAULT (datetime('now')),
  started_at TEXT,
  completed_at TEXT
);
CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status);
CREATE INDEX IF NOT EXISTS idx_tasks_agent ON tasks(agent);
SCHEMA

# Insert Task 1 — market_analyst
sqlite3 "$DB_PATH" "INSERT INTO tasks (agent, task_type, status, prompt) VALUES ('market_analyst', 'routine_scan', 'pending', '$(echo "$TASK1_PROMPT" | sed "s/'/''/g")');"

# Insert Task 2 — bot_analyst
sqlite3 "$DB_PATH" "INSERT INTO tasks (agent, task_type, status, prompt) VALUES ('bot_analyst', 'routine_scan', 'pending', '$(echo "$TASK2_PROMPT" | sed "s/'/''/g")');"

ok "Seeded 2 tasks"
sqlite3 "$DB_PATH" "SELECT id, agent, task_type, status FROM tasks;" | while IFS='|' read -r id agent ttype status; do
    info "  Task $id: $agent / $ttype [$status]"
done

# ─── 4. Start daemon ──────────────────────────────────────────────────────────
echo ""
echo -e "${GREEN}════════════════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}  Smoke test started. Press Ctrl+C to stop the daemon.${NC}"
echo -e "${GREEN}  Estimated cost: \$0.15-0.30${NC}"
echo -e "${GREEN}════════════════════════════════════════════════════════════════${NC}"
echo ""

# Trap SIGINT to print results after user stops daemon
print_results() {
    echo ""
    echo -e "${CYAN}════════════════════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Smoke Test Results${NC}"
    echo -e "${CYAN}════════════════════════════════════════════════════════════════${NC}"
    echo ""

    if [ ! -f "$DB_PATH" ]; then
        error "Database not found at $DB_PATH"
        exit 1
    fi

    TOTAL=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM tasks;" 2>/dev/null || echo "?")
    DONE=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM tasks WHERE status = 'done';" 2>/dev/null || echo "?")
    FAILED=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM tasks WHERE status = 'failed';" 2>/dev/null || echo "?")
    ACTIVE=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM tasks WHERE status = 'active';" 2>/dev/null || echo "?")
    PENDING=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM tasks WHERE status = 'pending';" 2>/dev/null || echo "?")
    COST=$(sqlite3 "$DB_PATH" "SELECT COALESCE(printf('%.4f', SUM(cost)), '0.0000') FROM tasks;" 2>/dev/null || echo "?")

    # Proposals (table may not exist if daemon never created it)
    PROPOSALS=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM proposals;" 2>/dev/null || echo "0")
    APPROVED=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM proposals WHERE state = 2;" 2>/dev/null || echo "0")
    REJECTED=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM proposals WHERE state = 3;" 2>/dev/null || echo "0")
    ESCALATED=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM proposals WHERE state = 4;" 2>/dev/null || echo "0")

    echo "  Tasks"
    echo "  ─────────────────────────────"
    echo "  Total:      $TOTAL"
    echo "  Completed:  $DONE"
    echo "  Failed:     $FAILED"
    echo "  Active:     $ACTIVE"
    echo "  Pending:    $PENDING"
    echo "  Total cost: \$$COST"
    echo ""
    echo "  Proposals"
    echo "  ─────────────────────────────"
    echo "  Created:    $PROPOSALS"
    echo "  Approved:   $APPROVED"
    echo "  Rejected:   $REJECTED"
    echo "  Escalated:  $ESCALATED"
    echo ""

    echo "  Vault Files Written"
    echo "  ─────────────────────────────"
    VAULT_COUNT=$(find "$DATA_DIR/vaults" -path "*/knowledge/*" -type f 2>/dev/null | wc -l | tr -d ' ')
    echo "  Count: $VAULT_COUNT"
    if [ "$VAULT_COUNT" -gt 0 ]; then
        find "$DATA_DIR/vaults" -path "*/knowledge/*" -type f 2>/dev/null | while read -r f; do
            echo "    - ${f#$ROOT_DIR/}"
        done
    fi
    echo ""

    echo "  Task Details"
    echo "  ─────────────────────────────"
    sqlite3 -header -column "$DB_PATH" \
        "SELECT id, agent, task_type, status, token_in, token_out, printf('%.4f', COALESCE(cost, 0)) AS cost_usd FROM tasks;" 2>/dev/null || echo "  (could not query tasks)"
    echo ""

    if [ "$PROPOSALS" -gt 0 ]; then
        echo "  Proposal Details"
        echo "  ─────────────────────────────"
        sqlite3 -header -column "$DB_PATH" \
            "SELECT id, title, author, state, current_round FROM proposals;" 2>/dev/null || echo "  (could not query proposals)"
        echo ""
    fi

    echo -e "${CYAN}════════════════════════════════════════════════════════════════${NC}"
}

trap print_results EXIT

# Run daemon in foreground — user watches verbose output and stops with Ctrl+C
"$DAEMON_BIN" --config "$CONFIG_PATH" --verbose
