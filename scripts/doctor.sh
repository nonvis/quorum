#!/bin/bash
# Read-only setup check for the Python-only Quorum. Exit 1 on any failure.
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail=0
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; fail=1; }

v=$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])' 2>/dev/null) \
    && ok "python3 $v" || bad "python3 not found"
python3 -c 'import sqlite3; c=sqlite3.connect(":memory:"); c.execute("create virtual table t using fts5(x)")' 2>/dev/null \
    && ok "sqlite3 FTS5" || bad "python3 sqlite3 lacks FTS5"
q=$(command -v quorum)
[ -n "$q" ] && [ "$(readlink "$q")" = "$REPO_ROOT/scripts/quorum_cli.py" ] \
    && ok "quorum on PATH -> scripts/quorum_cli.py" || bad "quorum not linked (run make install)"
for s in sui-dev-skills move-code-quality cpp-code-quality; do
    [ "$(readlink "$HOME/.claude/skills/$s")" = "$REPO_ROOT/templates/skills/$s" ] \
        && ok "skill $s linked" || bad "skill $s not linked (run make install)"
done
for l in "$HOME/.claude/skills/quorum-roles" "$HOME/.claude/agents/supervisor.md"; do
    [ -L "$l" ] && bad "retired link still present: $l (run make install)"
done
[ $fail = 0 ] && echo "0 failed" || echo "FAILED"
exit $fail
