#!/usr/bin/env bash
#
# setup-advisor.sh <project-dir>
#
# Scaffold the OPTIONAL, cost-gated Quorum "advisor" agent into a project. The
# advisor is NOT a new role — it is a `role: thinker` agent carrying the advisor
# SKILL (templates/skills/advisor/SKILL.md). It gives planning READ-ONLY access
# to the operator's EXTERNAL second-brain markdown vault (Obsidian/PARA). The
# per-operator vault path lives in <project-dir>/.quorum/config.yaml under
# `advisor:` / `vault_path:`.
#
# What this script does:
#   1. Resolves the QUORUM repo + project dir; requires build/quorum_daemon.
#   2. Requires <project-dir>/.quorum (run `quorum init` first if absent).
#   3. Reads advisor.vault_path from .quorum/config.yaml.
#      - If ABSENT/empty: WARN, create an INERT advisor, EXIT 0 (graceful).
#   4. Creates the advisor agent token-free (--no-ai; idempotent).
#   5. Gathers cheap vault metadata (folder tree + per-note `summary:`; NO bodies).
#   6. Runs ONE token-spending LLM pass to derive the `## Vault Scope` (the ONLY
#      token cost — it prints a notice first).
#   7. Splices that scope into the advisor's CONTEXT.md.
#
# Idempotent: re-running skips an existing advisor agent. NO state-mutating git
# in the target. NEVER hardcodes a vault path — it is per-operator config.
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

ADVISOR_SKILL="$QUORUM/templates/skills/advisor/SKILL.md"
SCAN_TOOL="$QUORUM/scripts/advisor_vault_scan.py"
for f in "$ADVISOR_SKILL" "$SCAN_TOOL"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: required source file missing: $f" >&2
        exit 1
    fi
done

# ── 2. Require .quorum/ ─────────────────────────────────────────────────────
CONFIG="$PROJECT_DIR/.quorum/config.yaml"
if [ ! -d "$PROJECT_DIR/.quorum" ]; then
    echo "ERROR: $PROJECT_DIR has no .quorum/ — run 'quorum init' there first" >&2
    exit 1
fi

ADVISOR_YAML="$PROJECT_DIR/.quorum/agents/advisor.yaml"
CONTEXT_MD="$PROJECT_DIR/.quorum/vaults/advisor/CONTEXT.md"

echo "==> Quorum advisor setup"
echo "    project : $PROJECT_DIR"
echo "    quorum  : $QUORUM"
echo "    daemon  : $DAEMON"
echo ""

# ── 3. Read advisor.vault_path from .quorum/config.yaml ─────────────────────
# Minimal YAML read: find the `advisor:` section, then `vault_path:` under it.
# Deterministic, no token cost. Tolerates the key being absent.
read_vault_path() {
    [ -f "$CONFIG" ] || return 0
    awk '
        /^[^[:space:]#]/ { in_advisor = ($1 == "advisor:") }
        in_advisor && $1 == "vault_path:" {
            sub(/^[[:space:]]*vault_path:[[:space:]]*/, "")
            gsub(/^"|"$/, "")
            gsub(/^'"'"'|'"'"'$/, "")
            print
            exit
        }
    ' "$CONFIG"
}
VAULT_PATH="$(read_vault_path || true)"

# ── 4. Create the advisor agent (token-free; idempotent) ────────────────────
DESC="Advisor: read-only planning context from the operator's external second-brain vault. Scope in its CONTEXT.md; soft/expandable. Read-only."
if [ -f "$ADVISOR_YAML" ]; then
    echo "==> advisor agent already exists — skipping create"
else
    echo "==> creating advisor agent (thinker + advisor skill, --no-ai)"
    ( cd "$PROJECT_DIR" && "$DAEMON" agent create \
        --role thinker \
        --name advisor \
        --skill-file "$ADVISOR_SKILL" \
        --description "$DESC" \
        --target-dir "$PROJECT_DIR" \
        --no-ai )
fi

if [ ! -f "$CONTEXT_MD" ]; then
    echo "ERROR: expected advisor CONTEXT.md not found: $CONTEXT_MD" >&2
    echo "       (agent create should have produced it from templates/agents/advisor.md)" >&2
    exit 1
fi

# splice_scope <body>: set the `## Vault Scope` section body in CONTEXT.md to
# <body>, preserving the rest of the file. The script OWNS this section.
#
# If a `## Vault Scope` heading already exists (e.g. CONTEXT.md was generated
# from templates/agents/advisor.md, which carries the {vault_scope} placeholder),
# its body is replaced. If it does NOT exist (the daemon falls back to a minimal
# scaffold when run with cwd=project, since templates/agents/advisor.md is
# resolved relative to cwd and isn't present there), the section is INSERTED —
# before `## Universal Rules` if present, else appended at end. Either way the
# advisor ends up with exactly one `## Vault Scope` section holding <body>.
splice_scope() {
    local body="$1"
    local tmp
    tmp="$(mktemp)"
    SCOPE_BODY="$body" awk '
        BEGIN { in_scope = 0; have_scope = 0 }
        /^## Vault Scope[[:space:]]*$/ {
            print
            print ENVIRON["SCOPE_BODY"]
            print ""
            in_scope = 1
            have_scope = 1
            next
        }
        # Insert before Universal Rules if we have not yet seen a Vault Scope.
        !have_scope && /^## Universal Rules[[:space:]]*$/ {
            print "## Vault Scope"
            print ENVIRON["SCOPE_BODY"]
            print ""
            have_scope = 1
        }
        in_scope && /^## / { in_scope = 0 }
        !in_scope { print }
        END {
            if (!have_scope) {
                print ""
                print "## Vault Scope"
                print ENVIRON["SCOPE_BODY"]
            }
        }
    ' "$CONTEXT_MD" > "$tmp"
    mv "$tmp" "$CONTEXT_MD"
}

# ── 3b. No vault path → inert advisor, EXIT 0 (graceful) ─────────────────────
if [ -z "${VAULT_PATH:-}" ]; then
    echo ""
    echo "WARNING: no advisor vault configured."
    echo "         Set 'vault_path' under an 'advisor:' section in:"
    echo "           $CONFIG"
    echo "         e.g.:"
    echo "           advisor:"
    echo "             vault_path: /absolute/path/to/your/second-brain"
    echo "         Then re-run: $0 \"$PROJECT_DIR\""
    echo ""
    INERT="No vault configured — advisor inert until \`advisor.vault_path\` is set in .quorum/config.yaml"
    splice_scope "$INERT"
    echo "==> advisor created INERT (no vault). Vault Scope written:"
    echo "      $INERT"
    echo "==> done (exit 0)."
    exit 0
fi

if [ ! -d "$VAULT_PATH" ]; then
    echo "WARNING: configured advisor vault_path is not a directory: $VAULT_PATH" >&2
    echo "         Leaving advisor inert. Fix the path in $CONFIG and re-run." >&2
    INERT="Configured vault_path does not exist: $VAULT_PATH — advisor inert until it resolves to a directory."
    splice_scope "$INERT"
    exit 0
fi

echo "==> advisor vault: $VAULT_PATH"
echo ""

# ── 5. Cheap vault metadata gather (folder tree + per-note summary) ─────────
echo "==> [scan] gathering cheap vault metadata (no note bodies read)"
DIGEST_FILE="$(mktemp)"
python3 "$SCAN_TOOL" "$VAULT_PATH" > "$DIGEST_FILE"

# Optional: include the project's identity so the scope is sharpened to relevance.
PROJECT_IDENTITY=""
if [ -f "$PROJECT_DIR/CLAUDE.md" ]; then
    PROJECT_IDENTITY="$(head -c 4000 "$PROJECT_DIR/CLAUDE.md")"
fi

# ── 6. ONE token-spending LLM pass → `## Vault Scope` ───────────────────────
PROMPT_FILE="$(mktemp)"
{
    echo "You are configuring a read-only planning 'advisor' for a software project."
    echo "The advisor reads the operator's external second-brain markdown vault to"
    echo "surface relevant prior thinking during planning."
    echo ""
    echo "Vault absolute path: $VAULT_PATH"
    echo ""
    if [ -n "$PROJECT_IDENTITY" ]; then
        echo "PROJECT IDENTITY (from the project's CLAUDE.md, for relevance):"
        echo "-----"
        echo "$PROJECT_IDENTITY"
        echo "-----"
        echo ""
    fi
    echo "VAULT DIGEST (folder tree + per-note summaries; no note bodies):"
    echo "-----"
    cat "$DIGEST_FILE"
    echo "-----"
    echo ""
    echo "TASK: Return a concise '## Vault Scope' markdown SECTION BODY (do NOT"
    echo "include the '## Vault Scope' heading itself). It must:"
    echo "  - Name the vault by its ABSOLUTE path: $VAULT_PATH"
    echo "  - List, as a few bullets, the folders/areas in this vault most relevant"
    echo "    to THIS project — each bullet an ABSOLUTE path under the vault root."
    echo "  - End with one line noting the scope is a soft efficiency boundary:"
    echo "    widen to out-of-zone notes when a thread clearly needs it."
    echo "Output ONLY the section body markdown — no preamble, no fences, no heading."
}> "$PROMPT_FILE"

echo ""
echo "==> [llm] running ONE token-spending scoping pass (this is the ONLY token cost)"
echo "    invoking: env -u CLAUDECODE cat <prompt> | claude -p --dangerously-skip-permissions \\"
echo "                --disallowedTools \"Write,Edit,NotebookEdit\" --output-format json"
echo ""

RAW="$(env -u CLAUDECODE cat "$PROMPT_FILE" | claude -p --dangerously-skip-permissions \
    --disallowedTools "Write,Edit,NotebookEdit" --output-format json 2>&1)" || {
        echo "ERROR: claude -p scoping pass failed:" >&2
        echo "$RAW" >&2
        rm -f "$DIGEST_FILE" "$PROMPT_FILE"
        exit 1
    }

# Extract the "result" field from the JSON envelope (python3, no jq dependency).
SCOPE="$(printf '%s' "$RAW" | python3 -c 'import sys, json
try:
    d = json.load(sys.stdin)
    print(d.get("result", "").strip())
except Exception:
    sys.exit(2)')" || {
        echo "ERROR: could not parse claude -p JSON output. Raw:" >&2
        echo "$RAW" >&2
        rm -f "$DIGEST_FILE" "$PROMPT_FILE"
        exit 1
    }

rm -f "$DIGEST_FILE" "$PROMPT_FILE"

if [ -z "$SCOPE" ]; then
    echo "ERROR: scoping pass returned an empty scope; leaving CONTEXT.md unchanged." >&2
    exit 1
fi

# ── 7. Splice the scope into CONTEXT.md ─────────────────────────────────────
splice_scope "$SCOPE"

# ── 8. Summary ──────────────────────────────────────────────────────────────
echo "==> advisor setup complete."
echo ""
echo "    vault path  : $VAULT_PATH"
echo "    agent yaml  : $ADVISOR_YAML"
echo "    context     : $CONTEXT_MD"
echo ""
echo "    Vault Scope written:"
echo "------------------------------------------------------------"
printf '%s\n' "$SCOPE"
echo "------------------------------------------------------------"
echo ""
echo "  NOTE: only the scoping pass spent tokens. Add 'advisor' to a team's"
echo "        default_path to use it in planning conversations."
