#!/usr/bin/env bash
#
# setup-knowers.sh <project-dir>
#
# Scaffold the read-only Quorum "knower" setup (cartographer + architect +
# historian + recap) into any project workspace. Idempotent: safe to re-run; never
# creates or edits the project's CLAUDE.md (it's external to Quorum), never
# duplicates agents, never runs state-mutating git in the target, and spends NO
# LLM tokens (Tier-1 scans are deterministic; agents created with --no-ai; no converse).
#
# After setup, run the (token-spending) Tier-2 LLM passes via:
#   scripts/run-knower.sh <project-dir> cartographer
#   scripts/run-knower.sh <project-dir> architect
#   scripts/run-knower.sh <project-dir> historian
#   scripts/run-knower.sh <project-dir> recap
#
set -euo pipefail

# ── Resolve args + paths ────────────────────────────────────────────────────
if [ "$#" -ne 1 ]; then
    echo "ERROR: usage: $0 <project-dir>" >&2
    exit 1
fi

PROJECT_DIR_RAW="$1"
if [ ! -d "$PROJECT_DIR_RAW" ]; then
    echo "ERROR: project dir does not exist: $PROJECT_DIR_RAW" >&2
    exit 1
fi
PROJECT_DIR="$(cd "$PROJECT_DIR_RAW" && pwd)"

QUORUM="$(cd "$(dirname "$0")/.." && pwd)"
DAEMON="$QUORUM/build/quorum_daemon"
if [ ! -x "$DAEMON" ]; then
    echo "ERROR: quorum_daemon not found at $DAEMON" >&2
    echo "       Build it first:  make build  (from $QUORUM)" >&2
    exit 1
fi

CARTO_SKILL="$QUORUM/templates/skills/cartographer/SKILL.md"
ARCH_SKILL="$QUORUM/templates/skills/architect/SKILL.md"
HIST_SKILL="$QUORUM/templates/skills/historian/SKILL.md"
RECAP_SKILL="$QUORUM/templates/skills/recap/SKILL.md"
CARTO_TOOL_SRC="$QUORUM/scripts/cartographer_index.py"
HIST_TOOL_SRC="$QUORUM/scripts/historian_mine.py"
RECAP_TOOL_SRC="$QUORUM/scripts/recap_mine.py"
for f in "$CARTO_SKILL" "$ARCH_SKILL" "$HIST_SKILL" "$RECAP_SKILL" "$CARTO_TOOL_SRC" "$HIST_TOOL_SRC" "$RECAP_TOOL_SRC"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: required source file missing: $f" >&2
        exit 1
    fi
done

echo "==> Quorum knowers setup"
echo "    project : $PROJECT_DIR"
echo "    quorum  : $QUORUM"
echo "    daemon  : $DAEMON"
echo ""

# ── 1. Init .quorum/ if absent ──────────────────────────────────────────────
if [ -d "$PROJECT_DIR/.quorum" ]; then
    echo "==> [1/9] .quorum/ already present — skipping init"
else
    echo "==> [1/9] init .quorum/ in project"
    ( cd "$PROJECT_DIR" && "$DAEMON" init )
fi

# ── 2. CLAUDE.md is the project's own — Quorum never creates or edits it ─────
#    (the cartographer reads it as optional survey context if present)
if [ -f "$PROJECT_DIR/CLAUDE.md" ]; then
    echo "==> [2/9] CLAUDE.md present — cartographer will read it as context (left untouched)"
else
    echo "==> [2/9] no CLAUDE.md — fine; it's the project's to own, not Quorum's to create"
fi

# ── 3. Refresh the Tier-1 tools into the project ────────────────────────────
echo "==> [3/9] refresh Tier-1 tools -> .quorum/tools/"
mkdir -p "$PROJECT_DIR/.quorum/tools"
cp "$CARTO_TOOL_SRC" "$PROJECT_DIR/.quorum/tools/cartographer_index.py"
chmod +x "$PROJECT_DIR/.quorum/tools/cartographer_index.py"
cp "$HIST_TOOL_SRC" "$PROJECT_DIR/.quorum/tools/historian_mine.py"
chmod +x "$PROJECT_DIR/.quorum/tools/historian_mine.py"
cp "$RECAP_TOOL_SRC" "$PROJECT_DIR/.quorum/tools/recap_mine.py"
chmod +x "$PROJECT_DIR/.quorum/tools/recap_mine.py"

# ── 4. Create the knower agents (idempotent: skip if yaml exists) ───────────
create_agent() {
    local name="$1" skill="$2" desc="$3"
    local yaml="$PROJECT_DIR/.quorum/agents/$name.yaml"
    if [ -f "$yaml" ]; then
        echo "    - $name agent already exists — skipping"
        return 0
    fi
    echo "    - creating $name agent (thinker, read-only, --no-ai)"
    # cwd = project dir so the daemon discovers THIS project's .quorum/.
    # --no-ai: copy template as-is, no claude -p call (zero token spend).
    ( cd "$PROJECT_DIR" && "$DAEMON" agent create \
        --role thinker \
        --name "$name" \
        --skill-file "$skill" \
        --description "$desc" \
        --target-dir "$PROJECT_DIR" \
        --no-ai )
}

echo "==> [4/9] create knower agents (if not present)"
create_agent cartographer "$CARTO_SKILL" \
    "Cartographer: knows the project layout. Reads the Tier-1 index (.quorum/cartographer/layout.json) + honors the root CLAUDE.md; produces a fast-lookup project index. Read-only."
create_agent architect "$ARCH_SKILL" \
    "Architect: maps component interconnections (imports, cross-repo calls, event flows) with file evidence; traces the primary flow; flags coupling/invariants. Read-only."
create_agent historian "$HIST_SKILL" \
    "Historian: knows the project's decisions + pivots. Reads the Tier-1 record (.quorum/historian/decisions-raw.json) + the Decision Log; tracks status/supersession with PR/commit provenance. Read-only."
create_agent recap "$RECAP_SKILL" \
    "Recap: knows what changed recently + where you left off (WHAT/WHEN). Reads the Tier-1 windowed timeline (.quorum/recap/timeline-raw.json) + operator-dumped timestamped messages, weaves one dated component-grouped timeline, drafts where-i-left-off, with a by-intent read-only Linear status overlay. Read-only; never queries Linear/Slack/Telegram."

# ── 5. Deterministic Tier-1 layout scan (cartographer; no tokens) ───────────
echo "==> [5/8] run deterministic Tier-1 layout scan (cartographer)"
python3 "$PROJECT_DIR/.quorum/tools/cartographer_index.py" --root "$PROJECT_DIR"

# ── 6. Tier-1 decision mine (historian; needs an authenticated gh) ──────────
# The historian Tier-1 miner shells out to `gh pr list` for PR data. The tool
# itself degrades gracefully on a missing git remote (empty PR lists), but it
# needs an authenticated `gh` to fetch PRs at all. If `gh` is missing or has no
# auth token, skip this step with a warning rather than failing setup — the
# operator can run historian_mine.py later once gh is authenticated.
echo "==> [6/8] run deterministic Tier-1 decision mine (historian)"
if ! command -v gh >/dev/null 2>&1; then
    echo "    WARNING: 'gh' not found — historian PR mining needs an authenticated gh."
    echo "             Skipping; run later:"
    echo "               python3 $PROJECT_DIR/.quorum/tools/historian_mine.py --root \"$PROJECT_DIR\""
elif [ -z "$(gh auth token 2>/dev/null)" ]; then
    echo "    WARNING: 'gh' is not authenticated (empty auth token) — historian PR mining needs it."
    echo "             Skipping; run later (after 'gh auth login'):"
    echo "               python3 $PROJECT_DIR/.quorum/tools/historian_mine.py --root \"$PROJECT_DIR\""
else
    python3 "$PROJECT_DIR/.quorum/tools/historian_mine.py" --root "$PROJECT_DIR"
fi

# ── 7. Tier-1 recap timeline mine + seed dump stubs ─────────────────────────
# The recap miner ALWAYS emits the git timeline (windowed commits + where-I-
# left-off facts); gh only ENRICHES it with merged/open PRs. So unlike the
# historian step above (which is gh-gated), we run it UNCONDITIONALLY — it
# degrades gracefully (empty PR lists) without an authenticated gh.
echo "==> [7/8] run deterministic Tier-1 timeline mine (recap) + seed dump channels"
python3 "$PROJECT_DIR/.quorum/tools/recap_mine.py" --root "$PROJECT_DIR"

# Seed the operator-owned dump stubs (timestamped messages + Linear export) ONLY
# if absent — NEVER overwrite an existing dump (the operator owns these channels;
# e.g. bastion already has linear-dump.md + where-i-left-off.md from its prototype).
mkdir -p "$PROJECT_DIR/.quorum/recap"
if [ ! -f "$PROJECT_DIR/.quorum/recap/messages-dump.md" ]; then
    echo "    - seeding messages-dump.md stub"
    cat > "$PROJECT_DIR/.quorum/recap/messages-dump.md" <<'EOF'
---
source: chat dumps (Slack / Telegram / Signal / any chat) — operator-pasted
owner: recap (TIMED overlay — joins the dated timeline; recap NEVER queries chat tools)
format: one entry per block; each STARTS with `YYYY-MM-DD HH:MM · <source> · <author>`; body on following lines
---

# recap · messages dump (timestamped)

<!-- Paste relevant messages below. One entry per block; blank line between entries.
Each entry STARTS with a stamp line:  YYYY-MM-DD HH:MM · <source> · <author>
Example:

2026-05-29 09:10 · slack#project · alice
  Decided to go address-based RBAC; caps version-lock is too risky on the hot path.

Each stamp's DATE places the message on the timeline next to git commits.
Malformed/un-stamped blocks are skipped silently and counted in recap's SUMMARY. -->
EOF
fi
if [ ! -f "$PROJECT_DIR/.quorum/recap/linear-dump.md" ]; then
    echo "    - seeding linear-dump.md stub"
    cat > "$PROJECT_DIR/.quorum/recap/linear-dump.md" <<'EOF'
---
source: Linear export — operator-pasted
owner: recap (UNTIMED status overlay; recap NEVER queries Linear — operator dumps, recap logs)
kind: untimed status overlay (NOT the dated git timeline; ticket timing is approximate)
dumped: <YYYY-MM-DD>
---

# recap · Linear log (paste tickets below)

<!-- Paste a Linear export. recap rolls the static text into a coarse
Done / In-Progress / Blocked overlay (+ a "decisions needed" set), kept SEPARATE
from the git timeline and surfaced ONLY when a query names Linear. recap never
runs `linear` (company policy). Use H2 status sections with bullets:
`- TICKET-ID Title · owner · component`. -->

## In Review

## In Progress

## Todo

## Done

## Backlog

## ⚠ Decisions needed
EOF
fi

# ── 8. Summary + next steps ─────────────────────────────────────────────────
echo ""
echo "==> [8/8] Setup complete."
echo ""
echo "  Produced:"
echo "    $PROJECT_DIR/.quorum/                          (Quorum workspace)"
echo "    $PROJECT_DIR/.quorum/agents/cartographer.yaml"
echo "    $PROJECT_DIR/.quorum/agents/architect.yaml"
echo "    $PROJECT_DIR/.quorum/agents/historian.yaml"
echo "    $PROJECT_DIR/.quorum/agents/recap.yaml"
echo "    $PROJECT_DIR/.quorum/tools/cartographer_index.py"
echo "    $PROJECT_DIR/.quorum/tools/historian_mine.py"
echo "    $PROJECT_DIR/.quorum/tools/recap_mine.py"
echo "    $PROJECT_DIR/.quorum/cartographer/layout.json  (Tier-1 deterministic layout index)"
if [ -f "$PROJECT_DIR/.quorum/historian/decisions-raw.json" ]; then
    echo "    $PROJECT_DIR/.quorum/historian/decisions-raw.json  (Tier-1 deterministic decision record)"
fi
if [ -f "$PROJECT_DIR/.quorum/recap/timeline-raw.json" ]; then
    echo "    $PROJECT_DIR/.quorum/recap/timeline-raw.json  (Tier-1 windowed timeline)"
fi
echo "    $PROJECT_DIR/.quorum/recap/{messages-dump.md,linear-dump.md}  (operator-owned dump channels)"
echo ""
echo "  Next steps:"
echo "    1. If the historian mine was skipped (no authenticated gh), run it"
echo "       once gh is ready:"
echo "         python3 $PROJECT_DIR/.quorum/tools/historian_mine.py --root \"$PROJECT_DIR\""
echo "    2. Run the Tier-2 LLM passes (these DO spend tokens):"
echo "         $QUORUM/scripts/run-knower.sh \"$PROJECT_DIR\" cartographer"
echo "         $QUORUM/scripts/run-knower.sh \"$PROJECT_DIR\" architect"
echo "         $QUORUM/scripts/run-knower.sh \"$PROJECT_DIR\" historian"
echo "         $QUORUM/scripts/run-knower.sh \"$PROJECT_DIR\" recap"
echo "    3. recap reads two operator-owned dumps (seeded as stubs if absent):"
echo "         $PROJECT_DIR/.quorum/recap/messages-dump.md   (timestamped chat -> timeline)"
echo "         $PROJECT_DIR/.quorum/recap/linear-dump.md      (Linear export -> untimed overlay)"
echo ""
echo "  NOTE: this setup spent zero tokens and ran no state-mutating git in the target."
