#!/bin/bash
# Validate all quorum templates for consistency
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
# TEMPLATES_DIR overrides the templates root so a scratch copy can be linted
# (gate mutations, CI sandboxes). Default = this repo's templates/.
TEMPLATES="${TEMPLATES_DIR:-$REPO_ROOT/templates}"
ERRORS=0
WARNINGS=0

# --- Role contract ---
# DRIVERS run other agents and emit no HANDOFF/SUMMARY blocks of their own
# (Agent Catalog, "Drivers vs workers"), so the worker block contract does not
# apply to them; they are linted against their own section headings instead.
# Add a role here only when it drives rather than produces.
DRIVER_ROLES=(supervisor)

# WORKERS must carry these literal strings somewhere in the SKILL.
WORKER_SECTIONS=("Block Formats" "HANDOFF" "SUMMARY")

# DRIVERS must carry these H2 headings — anchored, so renaming a section reds.
DRIVER_HEADINGS=(
    "^## .*Startup gate"
    "^## .*Output Parity"
    "^## .*Stop conditions"
    "^## .*Hard rules"
)

is_driver() {
    local role="$1" d
    for d in "${DRIVER_ROLES[@]}"; do
        [ "$role" = "$d" ] && return 0
    done
    return 1
}

echo "Linting templates..."
echo "  templates root: $TEMPLATES"
echo ""

if [ ! -d "$TEMPLATES" ]; then
    echo "  ❌ templates root does not exist: $TEMPLATES"
    echo ""
    echo "Found 1 error(s), 0 warning(s)."
    exit 1
fi

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
role_count=0
for skill in "$TEMPLATES"/skills/quorum-roles/*/SKILL.md; do
    [ -f "$skill" ] || continue
    role_count=$((role_count + 1))
    role=$(basename "$(dirname "$skill")")
    check_frontmatter "$skill" "quorum-roles/$role"

    lines=$(wc -l < "$skill" | tr -d ' ')
    if [ "$lines" -gt 500 ]; then
        echo "  ⚠️  quorum-roles/$role — $lines lines (target: <500)"
        WARNINGS=$((WARNINGS + 1))
    fi

    if is_driver "$role"; then
        for heading in "${DRIVER_HEADINGS[@]}"; do
            if ! grep -Eq "$heading" "$skill"; then
                echo "  ❌ quorum-roles/$role (driver) — no H2 matching '$heading'"
                ERRORS=$((ERRORS + 1))
            fi
        done
        echo "  ✅ quorum-roles/$role [driver] ($lines lines)"
    else
        for section in "${WORKER_SECTIONS[@]}"; do
            if ! grep -q "$section" "$skill"; then
                echo "  ❌ quorum-roles/$role — missing '$section' section"
                ERRORS=$((ERRORS + 1))
            fi
        done
        echo "  ✅ quorum-roles/$role [worker] ($lines lines)"
    fi
done

# A glob that matches nothing is a silent pass — make it an error.
if [ "$role_count" -eq 0 ]; then
    echo "  ❌ quorum-roles — no role SKILL.md matched $TEMPLATES/skills/quorum-roles/*/SKILL.md"
    ERRORS=$((ERRORS + 1))
fi

# Check router SKILL.md
if [ -f "$TEMPLATES/skills/quorum-roles/SKILL.md" ]; then
    check_frontmatter "$TEMPLATES/skills/quorum-roles/SKILL.md" "quorum-roles/SKILL.md (router)"
    echo "  ✅ quorum-roles/SKILL.md (router)"
else
    echo "  ❌ quorum-roles/SKILL.md (router) — MISSING"
    ERRORS=$((ERRORS + 1))
fi

echo "  → $role_count role skill file(s)"
echo ""

# --- 2. Domain skills ---
# Everything under templates/skills/ that is not a quorum role.
echo "=== Domain Skills ==="
domain_list=""
if [ -d "$TEMPLATES/skills" ]; then
    domain_list=$(find "$TEMPLATES/skills" -mindepth 2 -name 'SKILL.md' -not -path '*/quorum-roles/*' | sort)
fi
domain_count=0
if [ -n "$domain_list" ]; then
    domain_count=$(printf '%s\n' "$domain_list" | wc -l | tr -d ' ')
fi

if [ "$domain_count" -eq 0 ]; then
    # Zero-match is one structural failure, not one per required file.
    echo "  ❌ domain skills — no SKILL.md under $TEMPLATES/skills (excluding quorum-roles)"
    ERRORS=$((ERRORS + 1))
else
    while IFS= read -r skill; do
        label="${skill#"$TEMPLATES/skills/"}"
        label="${label%/SKILL.md}"
        check_frontmatter "$skill" "$label"
        lines=$(wc -l < "$skill" | tr -d ' ')
        echo "  ✅ $label ($lines lines)"
    done <<< "$domain_list"

    # The glob above shrinks silently when a required skill is deleted or
    # renamed, so pin the ones the daemon and the specialties depend on.
    for required in sui-dev-skills \
                    sui-dev-skills/sui-move \
                    sui-dev-skills/sui-ts-sdk \
                    sui-dev-skills/sui-frontend \
                    move-code-quality \
                    cpp-code-quality; do
        if [ ! -f "$TEMPLATES/skills/$required/SKILL.md" ]; then
            echo "  ❌ $required/SKILL.md — MISSING"
            ERRORS=$((ERRORS + 1))
        fi
    done
fi

echo "  → $domain_count domain skill file(s)"
echo ""

# --- 3. Agent templates ---
echo "=== Agent Templates ==="
agent_count=0
for tmpl in "$TEMPLATES"/agents/*.md; do
    [ -f "$tmpl" ] || continue
    agent_count=$((agent_count + 1))
    name=$(basename "$tmpl" .md)

    if ! grep -q "{agent_name}" "$tmpl"; then
        echo "  ❌ agents/$name.md — missing {agent_name} placeholder"
        ERRORS=$((ERRORS + 1))
    fi

    echo "  ✅ agents/$name.md"
done

if [ "$agent_count" -eq 0 ]; then
    echo "  ❌ agents — no template matched $TEMPLATES/agents/*.md"
    ERRORS=$((ERRORS + 1))
fi

echo "  → $agent_count agent template(s)"
echo ""

# --- Summary ---
if [ "$ERRORS" -gt 0 ]; then
    echo "Found $ERRORS error(s), $WARNINGS warning(s)."
    exit 1
else
    echo "All templates valid. ($WARNINGS warning(s))"
fi
