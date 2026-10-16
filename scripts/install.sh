#!/bin/bash
# Install Quorum's user-facing pieces as SYMLINKS into this checkout, so
# `git pull` is the whole update:
#   ~/.local/bin/quorum           -> scripts/quorum_cli.py
#   ~/.claude/skills/<skill>      -> templates/skills/<skill>  (the three below)
# It also removes links left by the retired daemon (quorum-roles skill,
# supervisor agent) when they point into this checkout.
#
#   install.sh              link everything
#   install.sh --uninstall  remove the links that point into this checkout
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BINDIR="${BINDIR:-$HOME/.local/bin}"
SKILLS_DST="$HOME/.claude/skills"
SKILLS=(sui-dev-skills move-code-quality cpp-code-quality)
RETIRED_LINKS=("$SKILLS_DST/quorum-roles" "$HOME/.claude/agents/supervisor.md")

unlink_ours() {
    local dst="$1"
    if [ -L "$dst" ]; then
        case "$(readlink "$dst")" in
            "$REPO_ROOT"/*) rm "$dst"; echo "  removed $dst" ;;
        esac
    fi
}

if [ "${1:-}" = "--uninstall" ]; then
    unlink_ours "$BINDIR/quorum"
    for s in "${SKILLS[@]}"; do unlink_ours "$SKILLS_DST/$s"; done
    exit 0
fi

mkdir -p "$BINDIR" "$SKILLS_DST"
ln -sfn "$REPO_ROOT/scripts/quorum_cli.py" "$BINDIR/quorum"
echo "  quorum -> $BINDIR/quorum"
for s in "${SKILLS[@]}"; do
    dst="$SKILLS_DST/$s"
    [ -d "$dst" ] && [ ! -L "$dst" ] && { echo "  kept $dst (a real directory, not a link)"; continue; }
    ln -sfn "$REPO_ROOT/templates/skills/$s" "$dst"
    echo "  skill $s"
done
for l in "${RETIRED_LINKS[@]}"; do unlink_ours "$l"; done
