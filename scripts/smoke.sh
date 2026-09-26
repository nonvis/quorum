#!/usr/bin/env bash
# Live smoke — SETUP.md step S10. One real `claude -p` round trip through the
# daemon, on a throwaway project in a temp dir. SPENDS TOKENS (one cartographer
# pass, about $1) and takes a few minutes.
#
# Run it on a new machine and whenever the Claude Code CLI updates: the daemon
# parses the CLI's JSON reply, and a CLI change once made it reject every
# healthy reply (Claude Code 2.1.x, fixed in 40d8500). On PASS the versions are
# recorded in ~/.local/state/quorum/last-smoke; scripts/doctor.sh S10 warns
# when the installed CLI no longer matches.
#
#   scripts/smoke.sh     (or: make smoke)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$REPO_ROOT/build/quorum_daemon"
[ -x "$BIN" ] || { echo "not built — make build" >&2; exit 1; }
command -v claude >/dev/null 2>&1 || { echo "claude not on PATH (SETUP.md S3)" >&2; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/quorum-smoke.XXXXXX")"
P="$WORK/smoke-proj"
fail() {
    echo "SMOKE FAILED: $*" >&2
    echo "  scratch project kept for inspection: $P" >&2
    exit 1
}

echo "==> scratch project: $P"
mkdir -p "$P/src"
cd "$P"
printf '# smoke-proj\n\nA throwaway C++ project for the Quorum live smoke.\n' > README.md
printf 'int main() { return 0; }\n' > src/main.cpp
printf 'cmake_minimum_required(VERSION 3.20)\nproject(smoke CXX)\nadd_executable(smoke src/main.cpp)\n' > CMakeLists.txt
git init -q
git config user.name "quorum-smoke"
git config user.email "smoke@localhost"
git add -A
git commit -qm init

echo "==> quorum init + Tier-1 knower setup (no tokens)"
"$BIN" init >/dev/null || fail "quorum init"
bash "$REPO_ROOT/scripts/setup-knowers.sh" "$P" >/dev/null 2>&1 || fail "setup-knowers.sh"

echo "==> cartographer refresh — the one claude -p round trip (spends tokens)"
"$BIN" knower refresh --knower cartographer --project "$P" || fail "knower refresh exited non-zero"

ref="$P/.quorum/vaults/cartographer/knowledge/ref-project-index.md"
[ -s "$ref" ] || fail "no $ref — the daemon did not accept the reply"
grep -q '^summary:' "$ref" || fail "$ref has no summary: line"
"$BIN" search "smoke" --project "$P" | grep -q 'ref-project-index' || fail "quorum search does not find the new ref"

claude_v="$(claude --version | awk '{print $1}')"
sha="$(git -C "$REPO_ROOT" rev-parse --short HEAD)"
state_dir="${XDG_STATE_HOME:-$HOME/.local/state}/quorum"
mkdir -p "$state_dir"
echo "claude=$claude_v quorum=$sha date=$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$state_dir/last-smoke"

rm -rf "$WORK"
echo "SMOKE PASSED — claude $claude_v, quorum $sha (recorded in $state_dir/last-smoke)"
