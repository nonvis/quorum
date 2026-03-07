#!/usr/bin/env bash
# Create a new Quorum agent with config and vault scaffolding.
# Usage: scripts/new_agent.sh <agent_id> [agent_name]
#
# Example:
#   scripts/new_agent.sh risk_manager "Risk Manager"

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [ $# -lt 1 ]; then
    echo "Usage: scripts/new_agent.sh <agent_id> [agent_name]"
    echo "  agent_id:   lowercase_snake_case identifier (e.g., risk_manager)"
    echo "  agent_name: display name in quotes (default: derived from agent_id)"
    exit 1
fi

AGENT_ID="$1"
AGENT_NAME="${2:-$(echo "$AGENT_ID" | sed 's/_/ /g' | awk '{for(i=1;i<=NF;i++) $i=toupper(substr($i,1,1)) substr($i,2)}1')}"

CONFIG_DIR="$REPO_ROOT/configs/agents"
VAULT_DIR="$REPO_ROOT/data/vaults/$AGENT_ID"
CONFIG_FILE="$CONFIG_DIR/$AGENT_ID.yaml"
CONTEXT_FILE="$VAULT_DIR/CONTEXT.md"
TEMPLATE="$REPO_ROOT/docs/templates/CONTEXT_TEMPLATE.md"

# Guard: don't overwrite existing agent
if [ -f "$CONFIG_FILE" ]; then
    echo "ERROR: Agent config already exists: $CONFIG_FILE"
    exit 1
fi

if [ -d "$VAULT_DIR" ]; then
    echo "ERROR: Agent vault already exists: $VAULT_DIR"
    exit 1
fi

# Create vault directory structure
mkdir -p "$VAULT_DIR"/{knowledge,experiments,decisions,inbox}

# Copy CONTEXT_TEMPLATE.md
if [ -f "$TEMPLATE" ]; then
    cp "$TEMPLATE" "$CONTEXT_FILE"
    echo "Created $CONTEXT_FILE (from template)"
else
    echo "WARNING: Template not found at $TEMPLATE — creating empty CONTEXT.md"
    echo "# $AGENT_NAME — Agent Context" > "$CONTEXT_FILE"
fi

# Generate agent YAML config
cat > "$CONFIG_FILE" << EOF
# Agent: $AGENT_NAME
# $(printf '=%.0s' $(seq 1 ${#AGENT_NAME}))===
# TODO: One-line description of what this agent does.
# TODO: What it looks at — e.g., "Looks at RISK — exposure, limits, alerts."

id: $AGENT_ID
name: "$AGENT_NAME"
description: "TODO: brief description"

vault_path: data/vaults/$AGENT_ID/
context_file: data/vaults/$AGENT_ID/CONTEXT.md

schedule:
  - type: periodic
    interval_minutes: 60
    task: routine_check

triggers: []

inference_tier:
  routine_check: 1                # local LLM (change to 2 for frontier)

context_budget:
  max_vault_files: 20
  max_vault_tokens: 50000
  always_include:
    - CONTEXT.md
  prefer_recent: true

boundaries:
  can_do:
    - TODO
  cannot_do:
    - TODO
EOF

echo "Created $CONFIG_FILE"
echo ""
echo "Next steps:"
echo "  1. Edit $CONFIG_FILE — fill in description, schedule, triggers, boundaries"
echo "  2. Edit $CONTEXT_FILE — fill in role, core question, data sources"
echo "  3. Restart daemon to pick up the new agent"
