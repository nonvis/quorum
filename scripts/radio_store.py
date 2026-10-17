#!/usr/bin/env python3
"""Radio's episode store (QR-19, QR-20), per host: <root>/<id>/{meta.json, script.json, script.md, audio.mp3}.

The root is ~/nonvis/quorum/.quorum/radio (QUORUM_RADIO_STORE overrides). An
episode is unkept when made; an unkept one is deleted 7 days after it was made,
swept whenever the store is listed (no timer). Keep makes it permanent; delete
is immediate. Python stdlib only.
"""
import json
import os
import re
import shutil
from datetime import datetime, timedelta, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
KST = timezone(timedelta(hours=9))
KEEP_DAYS = 7
KINDS = ("recap", "brainstorm")
PROJECT_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,39}$")
ID_RE = re.compile(r"^\d{8}-\d{6}-(recap|brainstorm)-[a-z0-9][a-z0-9-]{0,39}(-\d+)?$")


def default_root(env=None):
    env = os.environ if env is None else env
    v = env.get("QUORUM_RADIO_STORE")
    return Path(v) if v else REPO / ".quorum" / "radio"


def _z(t):
    return t.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _parse(s):
    return datetime.fromisoformat(s.replace("Z", "+00:00"))


def _alive(pid):
    try:
        os.kill(int(pid), 0)
    except (OSError, ValueError, TypeError):
        return False
    return True


class Store:
    def __init__(self, root=None):
        self.root = Path(root) if root else default_root()

    def _dir(self, i):
        if not isinstance(i, str) or not ID_RE.match(i):
            raise KeyError(i)
        d = self.root / i
        if not (d / "meta.json").exists():
            raise KeyError(i)
        return d

    def _write(self, path, text):
        tmp = path.with_name(path.name + ".tmp")
        tmp.write_text(text)
        os.replace(tmp, path)

    def new(self, kind, project, now, meta=None):
        if kind not in KINDS:
            raise ValueError("kind must be one of %s" % ", ".join(KINDS))
        if not PROJECT_RE.match(project or ""):
            raise ValueError("not a project id: %r" % project)
        self.root.mkdir(parents=True, exist_ok=True)
        base = "%s-%s-%s" % (now.astimezone(KST).strftime("%Y%m%d-%H%M%S"), kind, project)
        i, n = base, 1
        while True:
            try:
                (self.root / i).mkdir()
                break
            except FileExistsError:
                n += 1
                i = "%s-%d" % (base, n)
        m = dict(meta or {})
        m.update({"id": i, "kind": kind, "project": project, "created": _z(now), "kept": False,
                  "status": m.get("status", "generating")})
        self._write(self.root / i / "meta.json", json.dumps(m, indent=1, ensure_ascii=False))
        return i

    def meta(self, i):
        return json.loads((self._dir(i) / "meta.json").read_text())

    def update(self, i, **fields):
        d = self._dir(i)
        m = json.loads((d / "meta.json").read_text())
        m.update(fields)
        self._write(d / "meta.json", json.dumps(m, indent=1, ensure_ascii=False))
        return m

    def write_script(self, i, script, md):
        d = self._dir(i)
        self._write(d / "script.json", json.dumps(script, indent=1, ensure_ascii=False))
        self._write(d / "script.md", md)

    def script(self, i):
        p = self._dir(i) / "script.json"
        return json.loads(p.read_text()) if p.exists() else None

    def script_md(self, i):
        p = self._dir(i) / "script.md"
        return p.read_text() if p.exists() else None

    TEXTS = ("room.md",)

    def write_text(self, i, name, text):
        if name not in self.TEXTS:
            raise KeyError(name)
        self._write(self._dir(i) / name, text)

    def text(self, i, name):
        if name not in self.TEXTS:
            raise KeyError(name)
        p = self._dir(i) / name
        return p.read_text() if p.exists() else None

    def audio(self, i):
        p = self._dir(i) / "audio.mp3"
        return p if p.exists() else None

    def audio_path(self, i):
        return self._dir(i) / "audio.mp3"

    def keep(self, i, kept=True):
        return self.update(i, kept=bool(kept))

    def delete(self, i):
        shutil.rmtree(self._dir(i))

    def list(self, now=None, sweep=True):
        """Every episode, newest first, with `expires` and `days_left` (None when kept);
        unkept episodes past their 7 days are deleted first."""
        now = now or datetime.now(timezone.utc)
        out = []
        if not self.root.is_dir():
            return out
        for d in self.root.iterdir():
            if not (d.is_dir() and ID_RE.match(d.name) and (d / "meta.json").exists()):
                continue
            try:
                m = json.loads((d / "meta.json").read_text())
                created = _parse(m["created"])
            except (ValueError, KeyError):
                continue
            expires = created + timedelta(days=KEEP_DAYS)
            if sweep and not m.get("kept") and now >= expires:
                shutil.rmtree(d, ignore_errors=True)
                continue
            if m.get("status") in ("generating", "editing", "voicing") and m.get("pid") and not _alive(m["pid"]):
                m["status"] = "failed"
            m["expires"] = None if m.get("kept") else _z(expires)
            m["days_left"] = None if m.get("kept") else max(0, (expires - now).days + (1 if (expires - now).seconds else 0))
            m["has_audio"] = (d / "audio.mp3").exists()
            out.append(m)
        out.sort(key=lambda m: (m["created"], m["id"]), reverse=True)
        return out
