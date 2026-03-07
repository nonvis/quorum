#!/usr/bin/env bash
# observation_test.sh — Phase 0.5 observation mode test
#
# Seeds real mm-bot observation tasks, starts the daemon, and prints
# a validation summary when stopped (Ctrl+C) or after timeout.
#
# Run from the quorum/ root directory:
#   chmod +x scripts/observation_test.sh
#   ./scripts/observation_test.sh
#
# Estimated cost: $1.00-2.00 (5 seed tasks + auto-scheduled review tasks)
# Duration: run for ~1 hour, then Ctrl+C

set -euo pipefail

# Guard: claude -p nesting is blocked inside Claude Code sessions
if [ -n "${CLAUDECODE:-}" ]; then
    echo -e "\033[0;31m[ERROR]\033[0m Cannot run inside a Claude Code session."
    echo -e "\033[0;31m[ERROR]\033[0m Run from a regular terminal: ./scripts/observation_test.sh"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CORE_DIR="$ROOT_DIR/quorum-core"
DATA_DIR="$ROOT_DIR/data"
DB_PATH="$DATA_DIR/quorum.db"
CONFIG_PATH="$ROOT_DIR/configs/quorum.yaml"
DAEMON_BIN="$CORE_DIR/build/quorum_daemon"
KNOWLEDGE_DIR="$DATA_DIR/knowledge"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${CYAN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; }
ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }

# ─── 1. Pre-flight checks ───────────────────────────────────────────────────
info "Pre-flight checks..."

# Check mm-bot is running
if pgrep -f "mm_bot.*--live" > /dev/null 2>&1; then
    ok "mm-bot is running"
else
    warn "mm-bot is NOT running — agents will analyze historical data only"
fi

# Check mm-bot DB exists
MM_DB="$ROOT_DIR/../mm-bot/data/mm_bot.db"
if [ -f "$MM_DB" ]; then
    SESSION_COUNT=$(sqlite3 "$MM_DB" "SELECT COUNT(*) FROM sessions;" 2>/dev/null || echo "?")
    ok "mm-bot DB found ($SESSION_COUNT sessions)"
else
    error "mm-bot DB not found at $MM_DB"
    exit 1
fi

# Check daemon binary
if [ ! -x "$DAEMON_BIN" ]; then
    info "Building quorum_daemon..."
    mkdir -p "$CORE_DIR/build"
    (cd "$CORE_DIR/build" && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)")
    if [ ! -x "$DAEMON_BIN" ]; then
        error "Build failed"
        exit 1
    fi
    ok "Build complete"
else
    ok "Daemon binary found"
fi

# ─── 2. Clean previous state ────────────────────────────────────────────────
info "Cleaning previous quorum.db..."
if [ -f "$DB_PATH" ]; then
    rm -f "$DB_PATH" "${DB_PATH}-wal" "${DB_PATH}-shm"
    ok "Removed existing DB"
fi
rm -f "${DB_PATH}-wal" "${DB_PATH}-shm" 2>/dev/null

# ─── 3. Seed observation tasks ──────────────────────────────────────────────
info "Seeding observation tasks..."
mkdir -p "$DATA_DIR"
mkdir -p "$KNOWLEDGE_DIR/inbox" "$KNOWLEDGE_DIR/library" "$KNOWLEDGE_DIR/archive"

# Create tasks table
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

# ─── Task 1: Bot Analyst — Session Review ────────────────────────────────────
# (context assembler will prepend CONTEXT.md + knowledge/ automatically)
TASK_SESSION_REVIEW="Analyze the most recent completed mm-bot session.

Query the database at ../mm-bot/data/mm_bot.db to get:
1. The last completed session (stopped_at IS NOT NULL, ORDER BY id DESC LIMIT 1)
2. All fills for that session (side, price_human, quantity_human, realized_pnl_sui)
3. Portfolio snapshots (regime, skew_ratio, portfolio_value_sui)

Produce:
- A VAULT_UPDATE to knowledge/latest_session.md with your analysis (P&L breakdown, adverse selection %, bid vs ask performance, regime during session)
- An OBSERVATION with any notable patterns you found
- If you see a clear parameter improvement, write a PROPOSAL requiring consensus from engineer

Keep analysis data-driven. Show numbers, not opinions."

# ─── Task 2: Market Analyst — Pool Scan ──────────────────────────────────────
TASK_POOL_SCAN="Assess the current DEEP/SUI market conditions.

Query ../mm-bot/data/mm_bot.db:
1. Recent fill prices: SELECT price_human, side, realized_pnl_sui FROM fills ORDER BY id DESC LIMIT 50
2. Price range over last 2 sessions
3. Portfolio regime history: SELECT mid_price, regime FROM portfolio_snapshots ORDER BY id DESC LIMIT 30

Query ../bot-manager/data/bot_manager.db:
1. Latest macro scores: SELECT * FROM macro_scores ORDER BY timestamp DESC LIMIT 5

Produce:
- A VAULT_UPDATE to knowledge/market_state.md with current market assessment
- An OBSERVATION about any unusual patterns (price moves, regime shifts, volume changes)
- If macro conditions suggest the bot should adjust posture, write a PROPOSAL requiring consensus from bot_analyst"

# ─── Task 3: Operator — Health Check ────────────────────────────────────────
TASK_HEALTH_CHECK="Check the health of all system components.

Run these checks:
1. Is mm-bot running? Run: pgrep -f 'mm_bot.*--live'
2. Latest session status: sqlite3 ../mm-bot/data/mm_bot.db 'SELECT id, started_at, stopped_at, total_fills, realized_pnl_sui FROM sessions ORDER BY id DESC LIMIT 3'
3. Checkpoint lag: sqlite3 ../mev-bot/data/mev_bot.db 'SELECT lag_ms FROM checkpoint_lag ORDER BY timestamp DESC LIMIT 5'
4. Portfolio balance: sqlite3 ../mm-bot/data/mm_bot.db 'SELECT deep_balance, sui_balance, portfolio_value_sui FROM portfolio_snapshots ORDER BY id DESC LIMIT 1'

Produce:
- A VAULT_UPDATE to knowledge/health_status.md with current status of all components
- An OBSERVATION if anything is abnormal (high lag, low balance, process down, stale data)
- If there's a critical issue, write a PROPOSAL for remediation"

# ─── Task 4: Market Analyst — Regime Assessment ─────────────────────────────
TASK_REGIME="Evaluate the current macro regime and its implications for our MM strategy.

Query ../bot-manager/data/bot_manager.db:
1. Macro score history: SELECT datetime(timestamp, 'unixepoch') as ts, composite, regime, btc_trend, funding, oi_momentum, tvl, fng FROM macro_scores ORDER BY timestamp DESC LIMIT 20
2. Check data freshness — is the latest timestamp recent or stale?

Query ../mm-bot/data/mm_bot.db:
1. Recent portfolio regimes: SELECT regime, COUNT(*) FROM portfolio_snapshots WHERE session_id = (SELECT MAX(id) FROM sessions) GROUP BY regime

Produce:
- A VAULT_UPDATE to knowledge/regime_assessment.md with your assessment
- An OBSERVATION about regime trends or data staleness issues
- If regime suggests the bot should change spread or pull quotes, PROPOSAL requiring consensus from bot_analyst"

# ─── Task 5: Bot Analyst — Parameter Evaluation ─────────────────────────────
TASK_PARAM_EVAL="Evaluate whether current mm-bot parameters are optimal based on recent performance.

Query ../mm-bot/data/mm_bot.db:
1. Last 5 sessions: SELECT id, total_fills, realized_pnl_sui, mtm_pnl_sui FROM sessions WHERE stopped_at IS NOT NULL ORDER BY id DESC LIMIT 5
2. Fill stats per session: SELECT session_id, side, COUNT(*) as fills, SUM(realized_pnl_sui) as pnl FROM fills WHERE session_id IN (SELECT id FROM sessions ORDER BY id DESC LIMIT 5) GROUP BY session_id, side
3. Adverse selection trend: SELECT session_id, COUNT(*) as total, SUM(CASE WHEN realized_pnl_sui < 0 THEN 1 ELSE 0 END) as adverse FROM fills WHERE session_id IN (SELECT id FROM sessions ORDER BY id DESC LIMIT 5) GROUP BY session_id

Current parameters are in your CONTEXT.md. Compare actual performance against what the parameters should produce.

Produce:
- A VAULT_UPDATE to knowledge/parameter_assessment.md with your evaluation
- An OBSERVATION about any trends in performance across sessions
- If you recommend a specific parameter change, PROPOSAL with data backing (requires consensus from engineer)"

# Insert tasks
insert_task() {
    local agent="$1" task_type="$2" prompt="$3"
    sqlite3 "$DB_PATH" "INSERT INTO tasks (agent, task_type, status, prompt) VALUES ('$agent', '$task_type', 'pending', '$(echo "$prompt" | sed "s/'/''/g")');"
}

insert_task "bot_analyst" "session_review" "$TASK_SESSION_REVIEW"
insert_task "market_analyst" "pool_scan" "$TASK_POOL_SCAN"
insert_task "operator" "health_check" "$TASK_HEALTH_CHECK"
insert_task "market_analyst" "regime_assessment" "$TASK_REGIME"
insert_task "bot_analyst" "parameter_eval" "$TASK_PARAM_EVAL"

ok "Seeded 5 tasks"
sqlite3 "$DB_PATH" "SELECT id, agent, task_type, status FROM tasks;" | while IFS='|' read -r id agent ttype status; do
    info "  Task $id: $agent / $ttype [$status]"
done

# ─── 4. Start daemon ────────────────────────────────────────────────────────
echo ""
echo -e "${GREEN}════════════════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}  Phase 0.5 Observation Test${NC}"
echo -e "${GREEN}  5 tasks seeded. Estimated cost: \$1.00-2.00${NC}"
echo -e "${GREEN}  Press Ctrl+C to stop and see results.${NC}"
echo -e "${GREEN}════════════════════════════════════════════════════════════════${NC}"
echo ""

# Trap to print results on exit
print_results() {
    echo ""
    echo -e "${CYAN}════════════════════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Observation Test Results${NC}"
    echo -e "${CYAN}════════════════════════════════════════════════════════════════${NC}"
    echo ""

    if [ ! -f "$DB_PATH" ]; then
        error "Database not found"
        exit 1
    fi

    TOTAL=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM tasks;" 2>/dev/null || echo "?")
    DONE=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM tasks WHERE status = 'done';" 2>/dev/null || echo "?")
    FAILED=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM tasks WHERE status = 'failed';" 2>/dev/null || echo "?")
    ACTIVE=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM tasks WHERE status = 'active';" 2>/dev/null || echo "?")
    PENDING=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM tasks WHERE status = 'pending';" 2>/dev/null || echo "?")
    COST=$(sqlite3 "$DB_PATH" "SELECT COALESCE(printf('%.4f', SUM(cost)), '0.0000') FROM tasks;" 2>/dev/null || echo "?")

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

    echo "  Vault Updates"
    echo "  ─────────────────────────────"
    VAULT_COUNT=$(find "$DATA_DIR/vaults" -path "*/knowledge/*" -type f 2>/dev/null | wc -l | tr -d ' ')
    echo "  Knowledge files: $VAULT_COUNT"
    find "$DATA_DIR/vaults" -path "*/knowledge/*" -type f -newer "$0" 2>/dev/null | while read -r f; do
        echo "    + ${f#$ROOT_DIR/}"
    done
    echo ""

    echo "  Observations (Knowledge Inbox)"
    echo "  ─────────────────────────────"
    OBS_COUNT=$(find "$KNOWLEDGE_DIR/inbox" -type f 2>/dev/null | wc -l | tr -d ' ')
    echo "  Count: $OBS_COUNT"
    find "$KNOWLEDGE_DIR/inbox" -type f 2>/dev/null | sort | while read -r f; do
        echo "    + $(basename "$f")"
    done
    echo ""

    echo "  Task Details"
    echo "  ─────────────────────────────"
    sqlite3 -header -column "$DB_PATH" \
        "SELECT id, agent, task_type, status, token_in, token_out, printf('%.4f', COALESCE(cost, 0)) AS cost_usd FROM tasks;" 2>/dev/null || echo "  (could not query)"
    echo ""

    if [ "$PROPOSALS" -gt 0 ]; then
        echo "  Proposal Details"
        echo "  ─────────────────────────────"
        sqlite3 -header -column "$DB_PATH" \
            "SELECT id, title, author, state, current_round FROM proposals;" 2>/dev/null || echo "  (could not query)"
        echo ""

        echo "  Proposal Content (first 200 chars each)"
        echo "  ─────────────────────────────"
        sqlite3 "$DB_PATH" \
            "SELECT id || ': ' || substr(content, 1, 200) FROM proposals;" 2>/dev/null
        echo ""
    fi

    echo -e "${CYAN}════════════════════════════════════════════════════════════════${NC}"
}

trap print_results EXIT

"$DAEMON_BIN" --config "$CONFIG_PATH" --verbose
