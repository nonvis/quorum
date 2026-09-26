#!/usr/bin/env bash
# Quorum doctor — checks this machine (and optionally one project) against the
# SETUP.md runbook. Every SETUP.md step has exactly one `step <ID>` below, and
# ctest test_setup_runbook fails when the two lists differ, so the runbook and
# this check cannot drift apart. Read-only and $0: it never builds, installs,
# writes, or calls a model.
#
#   scripts/doctor.sh                    machine steps (S*)
#   scripts/doctor.sh --project <dir>    machine steps + project steps (P*) for <dir>
#
# Exit 0 when nothing FAILED (warnings and manual steps allowed), else 1.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$REPO_ROOT/build/quorum_daemon"
PROJECT=""
if [ "${1:-}" = "--project" ]; then
    PROJECT="${2:?--project needs a directory}"
    PROJECT="$(cd "$PROJECT" 2>/dev/null && pwd)" || { echo "no such directory: $2" >&2; exit 2; }
fi

fails=0
warns=0
CUR=""
step()   { CUR="$1"; }
ok()     { printf '  ok    %-4s %s\n' "$CUR" "$*"; }
fail()   { printf '  FAIL  %-4s %s\n' "$CUR" "$*"; fails=$((fails + 1)); }
warn()   { printf '  warn  %-4s %s\n' "$CUR" "$*"; warns=$((warns + 1)); }
manual() { printf '  todo  %-4s %s\n' "$CUR" "$*"; }

# joined <items...>: "a; b; c"
joined() { local out="" item; for item in "$@"; do out="${out:+$out; }$item"; done; printf '%s' "$out"; }

# version_ge <have> <need>
version_ge() { [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" = "$2" ]; }

echo "Quorum doctor — $REPO_ROOT (runbook: SETUP.md)"
echo ""
echo "Machine"

step S1 # toolchain
missing=()
for t in c++ cmake make git python3; do
    command -v "$t" >/dev/null 2>&1 || missing+=("$t")
done
if [ ${#missing[@]} -gt 0 ]; then
    fail "missing: ${missing[*]}"
else
    cmake_v="$(cmake --version | head -1 | awk '{print $3}')"
    if ! version_ge "$cmake_v" 3.20; then
        fail "cmake $cmake_v is older than 3.20"
    elif ! printf '#include <string>\nint main() { return std::string("ab").starts_with("a") ? 0 : 1; }\n' \
            | c++ -std=c++20 -x c++ -fsyntax-only - 2>/dev/null; then
        fail "c++ cannot compile C++20 ($(c++ --version | head -1))"
    else
        ok "$(c++ --version | head -1) · cmake $cmake_v · $(python3 --version)"
    fi
fi

step S2 # libraries
inc=()
if command -v brew >/dev/null 2>&1; then
    for f in openssl@3 sqlite curl; do
        p="$(brew --prefix "$f" 2>/dev/null)" && inc+=("-I$p/include")
    done
fi
missing=()
for h in curl/curl.h openssl/evp.h sqlite3.h; do
    printf '#include <%s>\nint main() {}\n' "$h" \
        | c++ -x c++ -fsyntax-only ${inc[@]+"${inc[@]}"} - 2>/dev/null || missing+=("$h")
done
if [ ${#missing[@]} -gt 0 ]; then fail "headers not found: ${missing[*]}"; else ok "curl, OpenSSL, SQLite headers"; fi

step S3 # Claude Code CLI
if ! command -v claude >/dev/null 2>&1; then
    fail "claude not on PATH"
elif ! claude auth status 2>/dev/null | grep -q '"loggedIn": *true'; then
    fail "claude $(claude --version 2>/dev/null | awk '{print $1}') is not signed in — claude auth login"
else
    ok "claude $(claude --version 2>/dev/null | awk '{print $1}'), signed in"
fi

step S4 # build
if [ ! -x "$BIN" ]; then
    fail "not built — make build"
else
    have="$("$BIN" version 2>/dev/null | head -1)"
    head="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null)"
    case "$have" in
        *"($head)"*)       ok "$have" ;;
        *"($head-dirty)"*) warn "$have — built from uncommitted changes" ;;
        *)                 warn "$have — checkout is at $head; make build" ;;
    esac
fi

step S5 # tests
manual "make test (C++) · cd quorum-web && bun test (web) — not run by doctor"

step S6 # install
bad=()
q="$(command -v quorum 2>/dev/null || true)"
if [ -z "$q" ]; then
    bad+=("quorum not on PATH")
elif [ "$(readlink -f "$q")" != "$(readlink -f "$BIN")" ]; then
    bad+=("quorum on PATH is $(readlink -f "$q"), not this checkout")
fi
for name in $("$REPO_ROOT/scripts/install-skills.sh" --list); do
    dst="$HOME/.claude/skills/$name"
    if [ -L "$dst" ] && [ "$(readlink "$dst")" = "$REPO_ROOT/templates/skills/$name" ]; then
        :
    elif [ -e "$dst" ]; then
        bad+=("~/.claude/skills/$name is not a link into this checkout")
    else
        bad+=("~/.claude/skills/$name missing")
    fi
done
agent="$HOME/.claude/agents/supervisor.md"
if ! { [ -L "$agent" ] && [ "$(readlink "$agent")" = "$REPO_ROOT/templates/skills/quorum-roles/supervisor/agent.md" ]; }; then
    bad+=("~/.claude/agents/supervisor.md is not a link into this checkout")
fi
if [ ${#bad[@]} -gt 0 ]; then
    fail "make install — $(joined "${bad[@]}")"
else
    ok "quorum on PATH; $("$REPO_ROOT/scripts/install-skills.sh" --list | wc -l | tr -d ' ') skills + supervisor agent linked"
fi

step S7 # GitHub CLI
if ! command -v gh >/dev/null 2>&1; then
    warn "gh not installed — setup-knowers.sh skips the historian scan"
elif [ -z "$(gh auth token 2>/dev/null)" ]; then
    warn "gh not signed in — gh auth login (setup-knowers.sh skips the historian scan)"
else
    ok "gh signed in"
fi

step S8 # web dashboard
bad=()
command -v bun >/dev/null 2>&1 || bad+=("bun not on PATH in this shell")
command -v tmux >/dev/null 2>&1 || bad+=("tmux missing (web flight launch)")
[ -d "$REPO_ROOT/quorum-web/node_modules" ] || bad+=("make web-install")
[ -d "$REPO_ROOT/quorum-web/client/node_modules" ] || bad+=("client deps missing (make web-install)")
if [ ${#bad[@]} -gt 0 ]; then warn "$(joined "${bad[@]}")"; else ok "bun $(bun --version), deps installed"; fi

step S9 # Docent
if python3 -c "import sqlite3; sqlite3.connect(':memory:').execute('create virtual table t using fts5(x)')" 2>/dev/null; then
    ok "python3 sqlite3 has FTS5"
else
    warn "python3's sqlite3 lacks FTS5 — Docent (quorum-own-agent) cannot index"
fi

step S10 # live smoke
state="${XDG_STATE_HOME:-$HOME/.local/state}/quorum/last-smoke"
claude_v="$(claude --version 2>/dev/null | awk '{print $1}')"
if [ ! -f "$state" ]; then
    warn "never run on this machine — make smoke (spends tokens)"
elif ! grep -q "claude=$claude_v " "$state"; then
    warn "claude is $claude_v but the last smoke passed on $(sed -n 's/.*claude=\([^ ]*\).*/\1/p' "$state") — make smoke"
else
    ok "passed on claude $claude_v ($(sed -n 's/.*date=\([^ ]*\).*/\1/p' "$state"))"
fi

if [ -n "$PROJECT" ]; then
    echo ""
    echo "Project $PROJECT"
    Q="$PROJECT/.quorum"

    step P1 # scaffolded
    if [ ! -f "$Q/config.yaml" ]; then
        fail "no .quorum/ — quorum init (from the project root)"
    elif ! git -C "$PROJECT" rev-parse --git-dir >/dev/null 2>&1; then
        warn ".quorum/ present but the project is not a git repo (the daemon auto-commits .quorum/**)"
    else
        ok ".quorum/ present, $(ls "$Q/agents"/*.yaml 2>/dev/null | wc -l | tr -d ' ') agents"
    fi

    step P2 # portable paths
    if [ -d "$Q/agents" ] && [ -x "$BIN" ]; then
        summary="$(cd "$PROJECT" && "$BIN" agent relink --dry-run 2>/dev/null | grep ' paths: ')"
        rewrite="$(sed -n 's/.* \([0-9]*\) rewritten.*/\1/p' <<<"$summary")"
        unresolved="$(sed -n 's/.* \([0-9]*\) unresolved.*/\1/p' <<<"$summary")"
        if [ "${rewrite:-x}" = "0" ] && [ "${unresolved:-x}" = "0" ]; then
            ok "$summary"
        else
            fail "${summary:-relink failed} — quorum agent relink (from the project root)"
        fi
    else
        fail "cannot check (no .quorum/agents or no build)"
    fi

    step P3 # leader role skill
    if grep -q '^skill_file:' "$Q/agents/leader.yaml" 2>/dev/null; then
        ok "leader.yaml has its role skill"
    else
        warn "leader.yaml has no skill_file — add: skill_file: ~/.claude/skills/quorum-roles/leader/SKILL.md"
    fi

    step P4 # Tier-1 knower inputs
    missing=()
    for f in cartographer/layout.json historian/decisions-raw.json recap/timeline-raw.json; do
        [ -f "$Q/$f" ] || missing+=("$f")
    done
    if [ ${#missing[@]} -gt 0 ]; then
        warn "missing ${missing[*]} — scripts/setup-knowers.sh $PROJECT"
    else
        ok "Tier-1 inputs present"
    fi

    step P5 # knower vaults
    empty=()
    oldest=0
    for k in cartographer architect historian recap; do
        refs=("$Q/vaults/$k/knowledge"/ref-*.md)
        if [ ! -f "${refs[0]}" ]; then
            empty+=("$k")
            continue
        fi
        newest="$(ls -t "${refs[@]}" | head -1)"
        age=$(( ( $(date +%s) - $(date -r "$newest" +%s) ) / 86400 ))
        [ "$age" -gt "$oldest" ] && oldest=$age
    done
    if [ ${#empty[@]} -gt 0 ]; then
        warn "empty: ${empty[*]} — knowledge is not in git; quorum knower refresh --all --project $PROJECT (spends tokens)"
    elif [ "$oldest" -gt 30 ]; then
        warn "stalest knower is ${oldest} days old — quorum knower refresh --all --project $PROJECT"
    else
        ok "4 knowers populated (stalest ${oldest} days)"
    fi

    step P6 # registered where you drive Quorum from
    manual "if you drive Quorum from a notes vault, add the project to its registry as a ~/ path"
fi

echo ""
echo "$fails failed, $warns warnings"
[ "$fails" -eq 0 ]
