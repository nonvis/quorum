#!/bin/bash
# Install all quorum skills to ~/.claude/skills/
# Installs: quorum-roles, sui-dev-skills, move-code-quality
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
SRC_DIR="$REPO_ROOT/templates/skills"
DST_DIR="$HOME/.claude/skills"
MODE="${1:-copy}"

install_skill() {
    local name="$1"
    local src="$SRC_DIR/$name"
    local dst="$DST_DIR/$name"

    if [ ! -d "$src" ]; then
        echo "  ❌ $name — source not found at $src"
        return 1
    fi

    if [ "$MODE" = "--link" ]; then
        ln -sfn "$src" "$dst"
        echo "  🔗 $name (symlinked)"
    else
        rm -rf "$dst"
        cp -r "$src" "$dst"
        echo "  📦 $name (copied)"
    fi
}

echo "Installing skills from $SRC_DIR → $DST_DIR"
echo ""

# Role skills
echo "Quorum role skills:"
install_skill "quorum-roles"
for role in leader thinker doer scribe reviewer librarian evaluator; do
    if [ -f "$DST_DIR/quorum-roles/$role/SKILL.md" ]; then
        echo "    ✅ $role ($(wc -l < "$DST_DIR/quorum-roles/$role/SKILL.md") lines)"
    else
        echo "    ❌ $role — MISSING"
    fi
done

echo ""

# Domain skills
echo "Domain skills:"
install_skill "sui-dev-skills"
for sub in sui-move sui-ts-sdk sui-frontend; do
    if [ -f "$DST_DIR/sui-dev-skills/$sub/SKILL.md" ]; then
        echo "    ✅ $sub ($(wc -l < "$DST_DIR/sui-dev-skills/$sub/SKILL.md") lines)"
    else
        echo "    ❌ $sub — MISSING"
    fi
done

install_skill "move-code-quality"

echo ""
echo "Done. All skills installed to $DST_DIR"
