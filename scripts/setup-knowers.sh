#!/usr/bin/env bash
#
# setup-knowers.sh <project-dir>
#
# Scaffold the read-only Quorum "knower" setup (cartographer + architect) into
# any project workspace. Idempotent: safe to re-run; never overwrites an
# existing CLAUDE.md, never duplicates agents, never runs git in the target,
# and spends NO LLM tokens (Tier-1 scan is deterministic; agents created with
# --no-ai; no converse).
#
# After setup, run the (token-spending) Tier-2 LLM passes via:
#   scripts/run-knower.sh <project-dir> cartographer
#   scripts/run-knower.sh <project-dir> architect
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
CLAUDE_TMPL="$QUORUM/templates/knowers/CLAUDE.template.md"
CARTO_TOOL_SRC="$QUORUM/scripts/cartographer_index.py"
for f in "$CARTO_SKILL" "$ARCH_SKILL" "$CLAUDE_TMPL" "$CARTO_TOOL_SRC"; do
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
    echo "==> [1/7] .quorum/ already present — skipping init"
else
    echo "==> [1/7] init .quorum/ in project"
    ( cd "$PROJECT_DIR" && "$DAEMON" init )
fi

# ── 2. Drop CLAUDE.md if absent (NEVER overwrite) ───────────────────────────
if [ -f "$PROJECT_DIR/CLAUDE.md" ]; then
    echo "==> [2/7] CLAUDE.md already present — leaving it untouched"
else
    echo "==> [2/7] copy CLAUDE.template.md -> $PROJECT_DIR/CLAUDE.md"
    cp "$CLAUDE_TMPL" "$PROJECT_DIR/CLAUDE.md"
fi

# ── 3. Refresh the Tier-1 tool into the project ─────────────────────────────
echo "==> [3/7] refresh cartographer_index.py -> .quorum/tools/"
mkdir -p "$PROJECT_DIR/.quorum/tools"
cp "$CARTO_TOOL_SRC" "$PROJECT_DIR/.quorum/tools/cartographer_index.py"
chmod +x "$PROJECT_DIR/.quorum/tools/cartographer_index.py"

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

echo "==> [4/7] create knower agents (if not present)"
create_agent cartographer "$CARTO_SKILL" \
    "Cartographer: knows the project layout. Reads the Tier-1 index (.quorum/cartographer/layout.json) + honors the root CLAUDE.md; produces a fast-lookup project index. Read-only."
create_agent architect "$ARCH_SKILL" \
    "Architect: maps component interconnections (imports, cross-repo calls, event flows) with file evidence; traces the primary flow; flags coupling/invariants. Read-only."

# ── 5. Write the knowers team (overwrite OK — it's our team) ────────────────
echo "==> [5/7] write .quorum/teams/knowers.yaml"
mkdir -p "$PROJECT_DIR/.quorum/teams"
cat > "$PROJECT_DIR/.quorum/teams/knowers.yaml" <<'EOF'
name: knowers
# Read-only "knower" team for analyzing this workspace.
# Run in brainstorm mode (clamps everyone to Read/Grep/Glob — no Bash, no writes).
default_path: [leader, cartographer, architect]
EOF

# ── 6. Deterministic Tier-1 scan (no tokens) ────────────────────────────────
echo "==> [6/7] run deterministic Tier-1 layout scan"
python3 "$PROJECT_DIR/.quorum/tools/cartographer_index.py" --root "$PROJECT_DIR"

# ── 7. Summary + next steps ─────────────────────────────────────────────────
echo ""
echo "==> [7/7] Setup complete."
echo ""
echo "  Produced:"
echo "    $PROJECT_DIR/.quorum/                          (Quorum workspace)"
echo "    $PROJECT_DIR/.quorum/agents/cartographer.yaml"
echo "    $PROJECT_DIR/.quorum/agents/architect.yaml"
echo "    $PROJECT_DIR/.quorum/teams/knowers.yaml"
echo "    $PROJECT_DIR/.quorum/tools/cartographer_index.py"
echo "    $PROJECT_DIR/.quorum/cartographer/layout.json  (Tier-1 deterministic index)"
if [ -f "$PROJECT_DIR/CLAUDE.md" ]; then
    echo "    $PROJECT_DIR/CLAUDE.md"
fi
echo ""
echo "  Next steps:"
echo "    1. Fill in the '## Folders' section of $PROJECT_DIR/CLAUDE.md"
echo "       (the cartographer honors it and produces the authoritative index)."
echo "    2. Run the Tier-2 LLM passes (these DO spend tokens):"
echo "         $QUORUM/scripts/run-knower.sh \"$PROJECT_DIR\" cartographer"
echo "         $QUORUM/scripts/run-knower.sh \"$PROJECT_DIR\" architect"
echo ""
echo "  NOTE: this setup spent zero tokens and ran no git in the target."
