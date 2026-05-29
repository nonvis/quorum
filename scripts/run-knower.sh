#!/usr/bin/env bash
#
# run-knower.sh <project-dir> <cartographer|architect>
#
# Run a single read-only "knower" Tier-2 LLM pass and produce its vault
# artifact. THIS SPENDS CLAUDE TOKENS (it runs `quorum converse`).
#
# Workaround for a known issue: `quorum converse` does NOT self-exit cleanly —
# it lingers after the conversation reaches `done`. So we launch it in the
# BACKGROUND, poll for the artifact to appear/update, flush, then kill the
# lingering process. (Follow-up: fix converse self-exit in the C++ daemon.)
#
# --mode brainstorm guarantees the agents are read-only (Read/Grep/Glob only —
# no Bash, no writes), so the target repos are never mutated.
#
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "ERROR: usage: $0 <project-dir> <cartographer|architect>" >&2
    exit 1
fi

PROJECT_DIR_RAW="$1"
KNOWER="$2"
if [ ! -d "$PROJECT_DIR_RAW" ]; then
    echo "ERROR: project dir does not exist: $PROJECT_DIR_RAW" >&2
    exit 1
fi
PROJECT_DIR="$(cd "$PROJECT_DIR_RAW" && pwd)"

QUORUM="$(cd "$(dirname "$0")/.." && pwd)"
DAEMON="$QUORUM/build/quorum_daemon"
if [ ! -x "$DAEMON" ]; then
    echo "ERROR: quorum_daemon not found at $DAEMON (run: make build)" >&2
    exit 1
fi

if [ ! -d "$PROJECT_DIR/.quorum" ]; then
    echo "ERROR: $PROJECT_DIR has no .quorum/ — run setup-knowers.sh first" >&2
    exit 1
fi

# ── Per-knower config: artifact path, budget, goal ──────────────────────────
case "$KNOWER" in
    cartographer)
        ARTIFACT="$PROJECT_DIR/.quorum/vaults/cartographer/knowledge/ref-project-index.md"
        BUDGET="2.0"
        GOAL="Produce/refresh the project layout index per your SKILL: read .quorum/cartographer/layout.json + the root CLAUDE.md (honor it), emit knowledge/ref-project-index.md, HANDOFF done."
        ;;
    architect)
        ARTIFACT="$PROJECT_DIR/.quorum/vaults/architect/knowledge/ref-architecture-map.md"
        BUDGET="3.0"
        GOAL="Map the component interconnections per your SKILL: read the cartographer index + CLAUDE.md, recover edges with file evidence, emit knowledge/ref-architecture-map.md, HANDOFF done."
        ;;
    *)
        echo "ERROR: unknown knower '$KNOWER' (expected: cartographer | architect)" >&2
        exit 1
        ;;
esac

# Record the artifact's pre-run mtime (or note absence).
if [ -f "$ARTIFACT" ]; then
    BEFORE_MTIME="$(stat -f %m "$ARTIFACT" 2>/dev/null || stat -c %Y "$ARTIFACT" 2>/dev/null || echo 0)"
    echo "==> $KNOWER artifact exists (mtime=$BEFORE_MTIME); will wait for an update"
else
    BEFORE_MTIME=""
    echo "==> $KNOWER artifact absent; will wait for it to appear"
fi
echo "    artifact : $ARTIFACT"
echo "    budget   : \$$BUDGET   mode: brainstorm (read-only)   team: knowers"
echo ""

# ── Launch converse in the BACKGROUND (it won't self-exit) ──────────────────
echo "==> launching converse (background) ..."
(
    cd "$PROJECT_DIR" && "$DAEMON" converse \
        --mode brainstorm \
        --team knowers \
        --budget "$BUDGET" \
        "$GOAL"
) >/dev/null 2>&1 &
CONVERSE_PID=$!
echo "    converse pid: $CONVERSE_PID"

cleanup() {
    kill "$CONVERSE_PID" >/dev/null 2>&1 || true
    pkill -f "build/quorum_daemon" >/dev/null 2>&1 || true
}
trap cleanup EXIT

# ── Poll up to ~15 min (90 × 10s) for the artifact to appear/update ─────────
artifact_ready() {
    [ -f "$ARTIFACT" ] || return 1
    if [ -z "$BEFORE_MTIME" ]; then
        return 0   # absent before, present now
    fi
    local now
    now="$(stat -f %m "$ARTIFACT" 2>/dev/null || stat -c %Y "$ARTIFACT" 2>/dev/null || echo 0)"
    [ "$now" -gt "$BEFORE_MTIME" ]
}

READY=0
for i in $(seq 1 90); do
    if artifact_ready; then
        echo "==> artifact updated (poll #$i); flushing ..."
        sleep 6
        READY=1
        break
    fi
    # Bail early if converse died before producing anything.
    if ! kill -0 "$CONVERSE_PID" 2>/dev/null && ! artifact_ready; then
        echo "==> converse process exited before producing the artifact (poll #$i)"
        break
    fi
    sleep 10
done

# ── Clean up the lingering converse process ─────────────────────────────────
echo "==> stopping converse (pid $CONVERSE_PID) + any lingering daemon"
kill "$CONVERSE_PID" >/dev/null 2>&1 || true
pkill -f "build/quorum_daemon" >/dev/null 2>&1 || true
trap - EXIT

# ── Report ──────────────────────────────────────────────────────────────────
echo ""
if [ -f "$ARTIFACT" ] && [ "$READY" -eq 1 ]; then
    echo "RESULT: PRESENT — $ARTIFACT"
    exit 0
elif [ -f "$ARTIFACT" ]; then
    echo "RESULT: PRESENT (not confirmed updated this run) — $ARTIFACT"
    exit 0
else
    echo "RESULT: MISSING — artifact not produced: $ARTIFACT" >&2
    echo "        (budget exhausted, agent error, or timeout — inspect manually)" >&2
    exit 1
fi
