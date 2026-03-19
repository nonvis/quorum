#!/bin/bash
# Validate all quorum templates for consistency
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
TEMPLATES="$REPO_ROOT/templates"
ERRORS=0
WARNINGS=0

echo "Linting templates..."
echo ""

# --- Helper ---
check_frontmatter() {
    local file="$1"
    local label="$2"

    if ! head -1 "$file" | grep -q "^---"; then
        echo "  ❌ $label — missing YAML frontmatter"
        ERRORS=$((ERRORS + 1))
        return 1
    fi
    if ! grep -q "^name:" "$file"; then
        echo "  ❌ $label — missing 'name' in frontmatter"
        ERRORS=$((ERRORS + 1))
    fi
    if ! grep -q "^description:" "$file"; then
        echo "  ❌ $label — missing 'description' in frontmatter"
        ERRORS=$((ERRORS + 1))
    fi
}

# --- 1. Quorum role skills ---
echo "=== Quorum Role Skills ==="
for skill in "$TEMPLATES"/skills/quorum-roles/*/SKILL.md; do
    role=$(basename "$(dirname "$skill")")
    check_frontmatter "$skill" "quorum-roles/$role"

    lines=$(wc -l < "$skill")
    if [ "$lines" -gt 500 ]; then
        echo "  ⚠️  quorum-roles/$role — $lines lines (target: <500)"
        WARNINGS=$((WARNINGS + 1))
    fi

    for section in "Block Formats" "HANDOFF" "SUMMARY"; do
        if ! grep -q "$section" "$skill"; then
            echo "  ❌ quorum-roles/$role — missing '$section' section"
            ERRORS=$((ERRORS + 1))
        fi
    done

    echo "  ✅ quorum-roles/$role ($lines lines)"
done

# Check router SKILL.md
if [ -f "$TEMPLATES/skills/quorum-roles/SKILL.md" ]; then
    check_frontmatter "$TEMPLATES/skills/quorum-roles/SKILL.md" "quorum-roles/SKILL.md (router)"
    echo "  ✅ quorum-roles/SKILL.md (router)"
fi

echo ""

# --- 2. Domain skills ---
echo "=== Domain Skills ==="

# sui-dev-skills router
if [ -f "$TEMPLATES/skills/sui-dev-skills/SKILL.md" ]; then
    check_frontmatter "$TEMPLATES/skills/sui-dev-skills/SKILL.md" "sui-dev-skills/SKILL.md (router)"
    echo "  ✅ sui-dev-skills/SKILL.md (router)"
else
    echo "  ❌ sui-dev-skills/SKILL.md (router) — MISSING"
    ERRORS=$((ERRORS + 1))
fi

# sui-dev sub-skills
for sub in sui-move sui-ts-sdk sui-frontend; do
    skill="$TEMPLATES/skills/sui-dev-skills/$sub/SKILL.md"
    if [ -f "$skill" ]; then
        check_frontmatter "$skill" "sui-dev-skills/$sub"
        lines=$(wc -l < "$skill")
        echo "  ✅ sui-dev-skills/$sub ($lines lines)"
    else
        echo "  ❌ sui-dev-skills/$sub — MISSING"
        ERRORS=$((ERRORS + 1))
    fi
done

# move-code-quality
if [ -f "$TEMPLATES/skills/move-code-quality/SKILL.md" ]; then
    check_frontmatter "$TEMPLATES/skills/move-code-quality/SKILL.md" "move-code-quality"
    lines=$(wc -l < "$TEMPLATES/skills/move-code-quality/SKILL.md")
    echo "  ✅ move-code-quality ($lines lines)"
else
    echo "  ❌ move-code-quality/SKILL.md — MISSING"
    ERRORS=$((ERRORS + 1))
fi

echo ""

# --- 3. Agent templates ---
echo "=== Agent Templates ==="
for tmpl in "$TEMPLATES"/agents/*.md; do
    name=$(basename "$tmpl" .md)

    if ! grep -q "{agent_name}" "$tmpl"; then
        echo "  ❌ agents/$name.md — missing {agent_name} placeholder"
        ERRORS=$((ERRORS + 1))
    fi

    echo "  ✅ agents/$name.md"
done

echo ""

# --- Summary ---
if [ "$ERRORS" -gt 0 ]; then
    echo "Found $ERRORS error(s), $WARNINGS warning(s)."
    exit 1
else
    echo "All templates valid. ($WARNINGS warning(s))"
fi
