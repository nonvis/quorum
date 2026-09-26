#!/bin/bash
# Install Quorum's Claude-Code-facing pieces under ~/.claude, as SYMLINKS into
# this checkout — so `git pull` here is the whole update, with no reinstall.
# SETUP.md step S6 is the runbook for this; scripts/doctor.sh checks it.
#
#   install-skills.sh              link every skill + the supervisor agent
#   install-skills.sh --copy       copy instead (a snapshot; drifts on pull)
#   install-skills.sh --list       print the installed skill names, one per line
#   install-skills.sh --uninstall  remove the links that point into this checkout
#
# Only what Claude Code itself must discover is installed. The knower lens
# skills (cartographer / architect / historian / recap / advisor) are NOT —
# agents reference them in place as $QUORUM/templates/skills/<name>/SKILL.md.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
SRC_DIR="$REPO_ROOT/templates/skills"
DST_DIR="$HOME/.claude/skills"
AGENTS_DST="$HOME/.claude/agents"
MODE="${1:-link}"

# The one list of installed skills (doctor.sh reads it via --list).
#   quorum-roles       role SKILLs the daemon injects (~/.claude/skills/quorum-roles/<role>)
#                      and the supervisor's own SKILL
#   sui-dev-skills,
#   move-code-quality,
#   cpp-code-quality   domain skills doers invoke by name from their `claude -p`
SKILLS=(quorum-roles sui-dev-skills move-code-quality cpp-code-quality)
SUPERVISOR_AGENT_SRC="$SRC_DIR/quorum-roles/supervisor/agent.md"
SUPERVISOR_AGENT_DST="$AGENTS_DST/supervisor.md"

if [ "$MODE" = "--list" ]; then
    printf '%s\n' "${SKILLS[@]}"
    exit 0
fi

# place <src> <dst>: link (default) or copy, replacing whatever is at <dst>.
# A real directory left by an older copy-install is removed first — `ln -sfn`
# onto an existing directory would create the link INSIDE it instead.
place() {
    local src="$1" dst="$2"
    if [ -d "$dst" ] && [ ! -L "$dst" ]; then
        rm -rf "$dst"
    fi
    if [ "$MODE" = "--copy" ]; then
        rm -rf "$dst"
        cp -r "$src" "$dst"
    else
        ln -sfn "$src" "$dst"
    fi
}

# unplace <dst>: remove <dst> only if it is a link into this checkout.
unplace() {
    local dst="$1" target
    if [ -L "$dst" ]; then
        target="$(readlink "$dst")"
        case "$target" in
            "$REPO_ROOT"/*) rm "$dst"; echo "    removed $dst" ;;
            *) echo "    kept $dst (links elsewhere: $target)" ;;
        esac
    elif [ -e "$dst" ]; then
        echo "    kept $dst (not a link — remove by hand if unwanted)"
    fi
}

if [ "$MODE" = "--uninstall" ]; then
    echo "Removing Quorum links from ~/.claude:"
    for name in "${SKILLS[@]}"; do unplace "$DST_DIR/$name"; done
    unplace "$SUPERVISOR_AGENT_DST"
    exit 0
fi

if [ "$MODE" != "link" ] && [ "$MODE" != "--copy" ]; then
    echo "usage: $0 [--copy | --list | --uninstall]" >&2
    exit 2
fi

verb="linked"; [ "$MODE" = "--copy" ] && verb="copied"
mkdir -p "$DST_DIR" "$AGENTS_DST"

echo "Installing skills from $SRC_DIR -> $DST_DIR ($verb)"
for name in "${SKILLS[@]}"; do
    if [ ! -d "$SRC_DIR/$name" ]; then
        echo "  ❌ $name — source not found at $SRC_DIR/$name"
        exit 1
    fi
    place "$SRC_DIR/$name" "$DST_DIR/$name"
    echo "  ✅ $name"
done

echo "Autopilot agent:"
if [ ! -f "$SUPERVISOR_AGENT_SRC" ]; then
    echo "  ❌ supervisor agent def — missing at $SUPERVISOR_AGENT_SRC"
    exit 1
fi
place "$SUPERVISOR_AGENT_SRC" "$SUPERVISOR_AGENT_DST"
echo "  ✅ supervisor -> $SUPERVISOR_AGENT_DST (claude --agent supervisor)"
