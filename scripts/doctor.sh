#!/bin/bash
# Read-only setup check for the Python-only Quorum. Exit 1 on any failure.
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail=0
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; fail=1; }
warn() { echo "  warn  $*"; }

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
# Radio: the cast file is checked like code; the voice sidecar is optional (warn only).
python3 -c "import sys; sys.path.insert(0, '$REPO_ROOT/scripts'); import radio; radio.load_cast()" 2>/dev/null \
    && ok "radio cast file + voice lock" || bad "radio/cast.json refused (python3 scripts/radio.py inputs says why)"
command -v claude >/dev/null && ok "claude on PATH (radio, overnight)" || warn "claude not on PATH: radio and overnight need --fake"
command -v node >/dev/null && [ -d "$REPO_ROOT/radio-voice/node_modules/kokoro-js" ] \
    && ok "radio-voice sidecar installed" || warn "radio-voice not installed (cd radio-voice && npm ci): radio needs --no-voice"
command -v ffmpeg >/dev/null && ok "ffmpeg" || warn "ffmpeg not found: radio needs --no-voice"
w="${QUORUM_KOKORO_CACHE:-$HOME/.cache/quorum/kokoro}/onnx-community/Kokoro-82M-v1.0-ONNX/onnx/model_quantized.onnx"
if [ -f "$w" ]; then
    [ "$(shasum -a 256 "$w" | cut -d' ' -f1)" = "$(cut -d' ' -f1 "$REPO_ROOT/radio-voice/weights.sha256")" ] \
        && ok "Kokoro weights match the pin" || warn "Kokoro weights differ from radio-voice/weights.sha256: the sidecar refuses them"
else
    warn "Kokoro weights not cached at $w (the first render downloads them)"
fi
# Radio's projects live in the vault (QR-24); Radio Studio's app is optional (warn only).
vault="${QUORUM_VAULT:-$HOME/nonvis/second-brain}"
python3 -c "import sys; sys.path.insert(0, '$REPO_ROOT/scripts'); import radio_projects; radio_projects.load('$vault')" 2>/dev/null \
    && ok "radio projects file in the vault (06_Metadata/quorum-projects.json)" \
    || bad "radio projects file missing or unreadable: $vault/06_Metadata/quorum-projects.json"
command -v npm >/dev/null && [ -d "$REPO_ROOT/radio-web/node_modules" ] \
    && ok "radio-web installed" || warn "radio-web not installed: quorum web start installs and builds it"
[ -f "$REPO_ROOT/radio-web/dist/index.html" ] && ok "radio-web built" || warn "radio-web not built: quorum web start builds it"
[ $fail = 0 ] && echo "0 failed" || echo "FAILED"
exit $fail
