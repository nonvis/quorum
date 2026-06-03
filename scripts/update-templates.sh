#!/bin/bash
# Review and update quorum templates using Claude
# Usage: ./scripts/update-templates.sh [role]
#   No args: review all templates
#   With role: review specific role (leader, thinker, doer, evaluator)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
TEMPLATES="$REPO_ROOT/templates"

ROLE="${1:-all}"

STYLE_GUIDE=$(cat <<'EOF'
Review this Quorum agent skill template for consistency and quality.

Style rules:
1. Frontmatter: must have name (quorum-{role}), description, user-invocable: false
2. First heading: # Quorum {Role} — Behavioral Patterns
3. Opening line: "You are a {role} agent. You {one-sentence purpose}."
4. Sections in order: Job description, Scope/Implementation Rules, Output Rules, Block Formats
5. Block Formats section must include HANDOFF, SUMMARY, and KNOWLEDGE with exact syntax
6. HANDOFF rules must include: never self-HANDOFF, standalone block at end
7. Use imperative form ("Read the plan" not "You should read the plan")
8. Explain why, not just what — reasoning > rigid MUSTs
9. Under 200 lines per skill (target ~120-150)
10. No HTML comments, no TODO markers

Report:
- Any missing required sections
- Inconsistencies with the style rules above
- Suggested improvements (be specific — show before/after)
- Line count assessment
EOF
)

review_template() {
    local skill_path="$1"
    local role="$2"

    echo "Reviewing $role skill..."
    echo "$STYLE_GUIDE" | cat - "$skill_path" | \
        claude -p --dangerously-skip-permissions \
        --disallowedTools "Write,Edit,NotebookEdit" \
        --output-format text 2>/dev/null
    echo ""
    echo "---"
}

if [ "$ROLE" = "all" ]; then
    for role in leader thinker doer evaluator; do
        skill="$TEMPLATES/skills/quorum-roles/$role/SKILL.md"
        if [ -f "$skill" ]; then
            review_template "$skill" "$role"
        fi
    done
else
    skill="$TEMPLATES/skills/quorum-roles/$ROLE/SKILL.md"
    if [ -f "$skill" ]; then
        review_template "$skill" "$ROLE"
    else
        echo "Template not found: $skill"
        exit 1
    fi
fi
