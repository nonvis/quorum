#!/usr/bin/env bash
#
# run-knower.sh <project-dir> <cartographer|architect>
#
# Run a single read-only "knower" Tier-2 LLM pass and produce its vault
# artifact. THIS SPENDS CLAUDE TOKENS (it runs `quorum converse`).
#
# `quorum converse` now self-exits cleanly when the conversation reaches a
# terminal state (exit-on-complete is the default; the old persistent mode is
# `--keep-alive`). That fix made the previous poll-for-artifact + pkill hack
# unnecessary: we just run converse and let it exit on its own. A background
# launch + generous wait-for-exit + last-resort kill remains only as a safety
# net in case converse somehow fails to exit; it is NOT the primary mechanism.
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

echo "==> running $KNOWER (read-only brainstorm pass) ..."
echo "    artifact : $ARTIFACT"
echo "    budget   : \$$BUDGET   mode: brainstorm (read-only)   team: knowers"
echo ""

# ── Run converse (happy path: it exits on its own when the conversation is
#    done). Background + wait-for-exit + last-resort kill is a safety net only.
(
    cd "$PROJECT_DIR" && "$DAEMON" converse \
        --mode brainstorm \
        --team knowers \
        --budget "$BUDGET" \
        "$GOAL"
) &
CONVERSE_PID=$!
echo "    converse pid: $CONVERSE_PID"

cleanup() {
    kill "$CONVERSE_PID" >/dev/null 2>&1 || true
    pkill -f "build/quorum_daemon" >/dev/null 2>&1 || true
}
trap cleanup EXIT

# Wait up to ~15 min (90 × 10s) for converse to self-exit.
SELF_EXITED=0
for i in $(seq 1 90); do
    if ! kill -0 "$CONVERSE_PID" 2>/dev/null; then
        SELF_EXITED=1
        break
    fi
    sleep 10
done

if [ "$SELF_EXITED" -eq 1 ]; then
    # Reap and capture the real exit status.
    wait "$CONVERSE_PID" 2>/dev/null || true
    echo "==> converse self-exited"
else
    # Safety net: converse did not exit on its own within ~15 min. This should
    # not happen with the converse-self-exit fix; kill it as a last resort.
    echo "==> WARNING: converse did not self-exit within ~15 min; killing (safety net)" >&2
    kill "$CONVERSE_PID" >/dev/null 2>&1 || true
    pkill -f "build/quorum_daemon" >/dev/null 2>&1 || true
fi
trap - EXIT

# ── Report ──────────────────────────────────────────────────────────────────
echo ""
if [ -f "$ARTIFACT" ]; then
    echo "RESULT: PRESENT — $ARTIFACT"
    exit 0
else
    echo "RESULT: MISSING — artifact not produced: $ARTIFACT" >&2
    echo "        (budget exhausted, agent error, or timeout — inspect manually)" >&2
    exit 1
fi
