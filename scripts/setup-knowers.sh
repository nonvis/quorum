#!/usr/bin/env bash
#
# setup-knowers.sh <project-dir>
#
# Scaffold the read-only Quorum "knower" setup (cartographer + architect +
# historian) into any project workspace. Idempotent: safe to re-run; never
# overwrites an existing CLAUDE.md, never duplicates agents, never runs
# state-mutating git in the target, and spends NO LLM tokens (Tier-1 scans are
# deterministic; agents created with --no-ai; no converse).
#
# After setup, run the (token-spending) Tier-2 LLM passes via:
#   scripts/run-knower.sh <project-dir> cartographer
#   scripts/run-knower.sh <project-dir> architect
#   scripts/run-knower.sh <project-dir> historian
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
CLAUDE_TMPL="$QUORUM/templates/knowers/CLAUDE.template.md"
CARTO_TOOL_SRC="$QUORUM/scripts/cartographer_index.py"
HIST_TOOL_SRC="$QUORUM/scripts/historian_mine.py"
for f in "$CARTO_SKILL" "$ARCH_SKILL" "$HIST_SKILL" "$CLAUDE_TMPL" "$CARTO_TOOL_SRC" "$HIST_TOOL_SRC"; do
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
    echo "==> [1/8] .quorum/ already present — skipping init"
else
    echo "==> [1/8] init .quorum/ in project"
    ( cd "$PROJECT_DIR" && "$DAEMON" init )
fi

# ── 2. Drop CLAUDE.md if absent (NEVER overwrite) ───────────────────────────
if [ -f "$PROJECT_DIR/CLAUDE.md" ]; then
    echo "==> [2/8] CLAUDE.md already present — leaving it untouched"
else
    echo "==> [2/8] copy CLAUDE.template.md -> $PROJECT_DIR/CLAUDE.md"
    cp "$CLAUDE_TMPL" "$PROJECT_DIR/CLAUDE.md"
fi

# ── 3. Refresh the Tier-1 tools into the project ────────────────────────────
echo "==> [3/8] refresh Tier-1 tools -> .quorum/tools/"
mkdir -p "$PROJECT_DIR/.quorum/tools"
cp "$CARTO_TOOL_SRC" "$PROJECT_DIR/.quorum/tools/cartographer_index.py"
chmod +x "$PROJECT_DIR/.quorum/tools/cartographer_index.py"
cp "$HIST_TOOL_SRC" "$PROJECT_DIR/.quorum/tools/historian_mine.py"
chmod +x "$PROJECT_DIR/.quorum/tools/historian_mine.py"

# ── 4. Create the two knower agents (idempotent: skip if yaml exists) ───────
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

echo "==> [4/8] create knower agents (if not present)"
create_agent cartographer "$CARTO_SKILL" \
    "Cartographer: knows the project layout. Reads the Tier-1 index (.quorum/cartographer/layout.json) + honors the root CLAUDE.md; produces a fast-lookup project index. Read-only."
create_agent architect "$ARCH_SKILL" \
    "Architect: maps component interconnections (imports, cross-repo calls, event flows) with file evidence; traces the primary flow; flags coupling/invariants. Read-only."
create_agent historian "$HIST_SKILL" \
    "Historian: knows the project's decisions + pivots. Reads the Tier-1 record (.quorum/historian/decisions-raw.json) + the Decision Log; tracks status/supersession with PR/commit provenance. Read-only."

# ── 5. Write the knowers team (overwrite OK — it's our team) ────────────────
echo "==> [5/8] write .quorum/teams/knowers.yaml"
mkdir -p "$PROJECT_DIR/.quorum/teams"
cat > "$PROJECT_DIR/.quorum/teams/knowers.yaml" <<'EOF'
name: knowers
# Read-only "knower" team for analyzing this workspace.
# Run in brainstorm mode (clamps everyone to Read/Grep/Glob — no Bash, no writes).
default_path: [leader, cartographer, architect, historian]
EOF

# ── 6. Deterministic Tier-1 layout scan (cartographer; no tokens) ───────────
echo "==> [6/8] run deterministic Tier-1 layout scan (cartographer)"
python3 "$PROJECT_DIR/.quorum/tools/cartographer_index.py" --root "$PROJECT_DIR"

# ── 7. Tier-1 decision mine (historian; needs an authenticated gh) ──────────
# The historian Tier-1 miner shells out to `gh pr list` for PR data. The tool
# itself degrades gracefully on a missing git remote (empty PR lists), but it
# needs an authenticated `gh` to fetch PRs at all. If `gh` is missing or has no
# auth token, skip this step with a warning rather than failing setup — the
# operator can run historian_mine.py later once gh is authenticated.
echo "==> [7/8] run deterministic Tier-1 decision mine (historian)"
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

# ── 8. Summary + next steps ─────────────────────────────────────────────────
echo ""
echo "==> [8/8] Setup complete."
echo ""
echo "  Produced:"
echo "    $PROJECT_DIR/.quorum/                          (Quorum workspace)"
echo "    $PROJECT_DIR/.quorum/agents/cartographer.yaml"
echo "    $PROJECT_DIR/.quorum/agents/architect.yaml"
echo "    $PROJECT_DIR/.quorum/agents/historian.yaml"
echo "    $PROJECT_DIR/.quorum/teams/knowers.yaml"
echo "    $PROJECT_DIR/.quorum/tools/cartographer_index.py"
echo "    $PROJECT_DIR/.quorum/tools/historian_mine.py"
echo "    $PROJECT_DIR/.quorum/cartographer/layout.json  (Tier-1 deterministic layout index)"
if [ -f "$PROJECT_DIR/.quorum/historian/decisions-raw.json" ]; then
    echo "    $PROJECT_DIR/.quorum/historian/decisions-raw.json  (Tier-1 deterministic decision record)"
fi
if [ -f "$PROJECT_DIR/CLAUDE.md" ]; then
    echo "    $PROJECT_DIR/CLAUDE.md"
fi
echo ""
echo "  Next steps:"
echo "    1. Fill in the '## Folders' section of $PROJECT_DIR/CLAUDE.md"
echo "       (the cartographer honors it and produces the authoritative index)."
echo "    2. If the historian mine was skipped (no authenticated gh), run it"
echo "       once gh is ready:"
echo "         python3 $PROJECT_DIR/.quorum/tools/historian_mine.py --root \"$PROJECT_DIR\""
echo "    3. Run the Tier-2 LLM passes (these DO spend tokens):"
echo "         $QUORUM/scripts/run-knower.sh \"$PROJECT_DIR\" cartographer"
echo "         $QUORUM/scripts/run-knower.sh \"$PROJECT_DIR\" architect"
echo "         $QUORUM/scripts/run-knower.sh \"$PROJECT_DIR\" historian"
echo ""
echo "  NOTE: this setup spent zero tokens and ran no state-mutating git in the target."
