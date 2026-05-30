#!/usr/bin/env bash
# Quorum web dashboard — start/stop the two-process dashboard in the background.
#
# The dashboard is two processes, both launched from quorum-web/:
#   - Hono API server (port 3100)         bun run dev
#   - Vite/React UI    (port 3101 → /api proxied to 3100)   bun run dev:client
#
# This runs BOTH in the background with one command, tracks PIDs, and tees logs,
# so you don't need two foreground terminals and can stop cleanly.
#
#   ./scripts/web.sh start      install deps if needed, launch both, print URLs
#   ./scripts/web.sh stop       stop both
#   ./scripts/web.sh status     report running state
#   ./scripts/web.sh restart    stop then start
#   ./scripts/web.sh logs       tail both logs
#
# PIDs + logs live under quorum-web/.web/ (gitignored).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
WEB_DIR="$REPO_ROOT/quorum-web"
CLIENT_DIR="$WEB_DIR/client"
RUN_DIR="$WEB_DIR/.web"
API_PID="$RUN_DIR/api.pid"
UI_PID="$RUN_DIR/ui.pid"
API_LOG="$RUN_DIR/api.log"
UI_LOG="$RUN_DIR/ui.log"
API_URL="http://localhost:3100"
UI_URL="http://localhost:3101"

mkdir -p "$RUN_DIR"

pid_alive() {  # pid_alive <pidfile>
  local f="$1" p
  [ -f "$f" ] || return 1
  p="$(cat "$f" 2>/dev/null || true)"
  [ -n "$p" ] && kill -0 "$p" 2>/dev/null
}

# start_one <label> <bun-script> <pidfile> <logfile>
start_one() {
  local label="$1" script="$2" pidf="$3" logf="$4"
  if pid_alive "$pidf"; then
    echo "  $label already running (pid $(cat "$pidf"))"
    return 0
  fi
  ( cd "$WEB_DIR" && nohup bun run "$script" >"$logf" 2>&1 & echo $! >"$pidf" )
  echo "  $label started (pid $(cat "$pidf")) — logs: ${logf#"$REPO_ROOT"/}"
}

# Recursively print a pid and ALL its descendants (root printed last). Snapshots
# the tree in one pass so reparenting on kill can't strand a grandchild — the UI
# is a 3-level tree (bun run dev:client -> bun run dev -> node/vite) and a
# single-level `pkill -P` would orphan the vite node still holding port 3101.
collect_tree() {
  local pid="$1" child
  for child in $(pgrep -P "$pid" 2>/dev/null || true); do
    collect_tree "$child"
  done
  echo "$pid"
}

# stop_one <label> <pidfile> — TERM then KILL the whole descendant tree.
stop_one() {
  local label="$1" pidf="$2" p q tree
  if pid_alive "$pidf"; then
    p="$(cat "$pidf")"
    tree="$(collect_tree "$p")"                       # full snapshot before any kill
    for q in $tree; do kill "$q" 2>/dev/null || true; done
    sleep 1
    for q in $tree; do kill -9 "$q" 2>/dev/null || true; done
    echo "  $label stopped (was pid $p)"
  else
    echo "  $label not running"
  fi
  rm -f "$pidf"
}

cmd_start() {
  command -v bun >/dev/null 2>&1 || { echo "ERROR: 'bun' not found. Install: curl -fsSL https://bun.sh/install | bash"; exit 1; }
  if [ ! -x "$REPO_ROOT/build/quorum_daemon" ]; then
    echo "NOTE: build/quorum_daemon not found — run 'make build' so the dashboard can"
    echo "      read the DB and drive the daemon. Starting the UI anyway."
  fi
  # Deps: server lives in quorum-web/, client in quorum-web/client/.
  [ -d "$WEB_DIR/node_modules" ]    || { echo "  installing API deps...";    ( cd "$WEB_DIR"    && bun install ); }
  [ -d "$CLIENT_DIR/node_modules" ] || { echo "  installing client deps..."; ( cd "$CLIENT_DIR" && bun install ); }
  echo "Starting Quorum web dashboard..."
  start_one "API" "dev"        "$API_PID" "$API_LOG"
  start_one "UI " "dev:client" "$UI_PID"  "$UI_LOG"
  echo ""
  echo "  Dashboard: $UI_URL   (API: $API_URL)"
  echo "  Status: ./scripts/web.sh status    Stop: ./scripts/web.sh stop"
}

cmd_stop() {
  echo "Stopping Quorum web dashboard..."
  stop_one "UI " "$UI_PID"
  stop_one "API" "$API_PID"
}

cmd_status() {
  if pid_alive "$API_PID"; then echo "API running (pid $(cat "$API_PID"))   $API_URL"; else echo "API stopped"; fi
  if pid_alive "$UI_PID";  then echo "UI  running (pid $(cat "$UI_PID"))   $UI_URL";  else echo "UI  stopped"; fi
}

cmd_logs() {
  echo "Tailing $RUN_DIR/*.log (Ctrl-C to stop)..."
  tail -n 40 -f "$API_LOG" "$UI_LOG"
}

case "${1:-}" in
  start)   cmd_start ;;
  stop)    cmd_stop ;;
  restart) cmd_stop; sleep 1; cmd_start ;;
  status)  cmd_status ;;
  logs)    cmd_logs ;;
  *) echo "Usage: $(basename "$0") {start|stop|status|restart|logs}"; exit 1 ;;
esac
