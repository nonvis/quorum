#!/usr/bin/env python3
"""Radio's projects file, in the vault (QR-24, QR-25): <vault>/06_Metadata/quorum-projects.json.

Each project maps to its briefing note(s), vault folder(s), repos and concept
note, plus `active` (a deactivated project is hidden, nothing deleted). Adding a
project also adds its repos to the index allowlist, and only the owner's own
github.com/nonvis repos are accepted (#99, from the remote URL). Every save
commits exactly the files it wrote, by pathspec, then pushes; a failed push is
reported and the commit stays. Python stdlib only.
"""
import fnmatch
import json
import re
import subprocess
from pathlib import Path

PROJECTS_REL = "06_Metadata/quorum-projects.json"
ALLOWLIST = "06_Metadata/quorum-repos.txt"
ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,39}$")
OWN_REMOTE = re.compile(r"github\.com[:/]nonvis/", re.I)
FIELDS = ("name", "notes", "folders", "repos", "concept")


class ProjectsError(Exception):
    def __init__(self, msg, errors=None):
        super().__init__(msg)
        self.errors = errors or [msg]


def _file(vault):
    return Path(vault) / PROJECTS_REL


def read_file(vault):
    p = _file(vault)
    if not p.exists():
        raise ProjectsError("no projects file at %s (Radio's projects live in the vault, %s)" % (p, PROJECTS_REL))
    return json.loads(p.read_text())


def load(vault, active_only=False):
    out = {}
    for pid, cfg in read_file(vault)["projects"].items():
        cfg = dict(cfg, active=cfg.get("active", True))
        if cfg["active"] or not active_only:
            out[pid] = cfg
    return out


def _safe(rel):
    rel = str(rel or "").strip()
    return bool(rel) and not rel.startswith(("/", "~")) and ".." not in rel.split("/") and "\x00" not in rel


def _vault_md(vault):
    v = Path(vault)
    return sorted(str(p.relative_to(v)) for p in v.rglob("*.md") if ".git" not in p.parts and ".quorum" not in p.parts)


def _remote(path):
    p = subprocess.run(["git", "-C", str(path), "remote", "get-url", "origin"], capture_output=True, text=True)
    return p.stdout.strip() if p.returncode == 0 else ""


def check(spec, vault, roots, existing=None):
    """Each path resolved, for the web's live check: {notes, concept: [{pattern, matches}],
    folders: [{path, exists}], repos: [{name, path, remote, own}], errors: [...]}."""
    errs = []
    pid = spec.get("id")
    if pid is not None:
        if not ID_RE.match(str(pid)):
            errs.append("id %r: lower-case letters, digits and dashes" % pid)
        elif existing and pid in existing:
            errs.append("a project %r already exists" % pid)
    if "name" in spec and not str(spec.get("name") or "").strip():
        errs.append("a name is needed")
    md = None
    out = {"notes": [], "concept": [], "folders": [], "repos": []}
    for key in ("notes", "concept"):
        for pat in spec.get(key) or []:
            if not _safe(pat):
                errs.append("%s: %r is not a path inside the vault" % (key, pat))
                out[key].append({"pattern": pat, "matches": []})
                continue
            md = md if md is not None else _vault_md(vault)
            hits = [r for r in md if fnmatch.fnmatchcase(r, pat) and r.count("/") == pat.count("/")]
            out[key].append({"pattern": pat, "matches": hits[:5]})
            if not hits:
                errs.append("%s: no note matches %r" % (key, pat))
    if "notes" in spec and not spec.get("notes"):
        errs.append("at least one briefing note is needed")
    if "folders" in spec and not spec.get("folders"):
        errs.append("at least one vault folder is needed")
    for f in spec.get("folders") or []:
        ok = _safe(f) and (Path(vault) / f).is_dir()
        out["folders"].append({"path": f, "exists": ok})
        if not ok:
            errs.append("folders: %r is not a folder in the vault" % f)
    for name in spec.get("repos") or []:
        row = {"name": name, "path": None, "remote": "", "own": False}
        if not ID_RE.match(str(name)) and not re.match(r"^[A-Za-z0-9._-]{1,60}$", str(name)):
            errs.append("repos: %r is not a repo name" % name)
        else:
            for r in roots:
                p = Path(r) / name
                if (p / ".git").exists():
                    row.update(path=str(p), remote=_remote(p))
                    row["own"] = bool(OWN_REMOTE.search(row["remote"]))
                    break
            if not row["path"]:
                errs.append("repos: %r is not a git repo under %s" % (name, ", ".join(str(r) for r in roots)))
            elif not row["own"]:
                errs.append("repos: %r is not one of your github.com/nonvis repos (#99; origin %s)"
                            % (name, row["remote"] or "none"))
        out["repos"].append(row)
    out["errors"] = errs
    return out


def _git(vault, *args):
    return subprocess.run(["git", "-C", str(vault)] + list(args), capture_output=True, text=True)


def _commit(vault, rels, message):
    """Commit exactly `rels`, by pathspec, then push (one rebase retry). Never raises on a push."""
    r = {"committed": None, "pushed": False, "push_error": None}
    if not (Path(vault) / ".git").exists():
        r["push_error"] = "the vault is not a git checkout; saved, not committed"
        return r
    _git(vault, "add", "--", *rels)
    c = _git(vault, "commit", "-q", "-m", message, "--", *rels)
    if c.returncode != 0:
        r["push_error"] = "commit failed: " + (c.stderr or c.stdout).strip()[-300:]
        return r
    r["committed"] = _git(vault, "rev-parse", "--short", "HEAD").stdout.strip()
    p = _git(vault, "push", "-q")
    if p.returncode != 0 and "rejected" in (p.stderr or ""):
        if _git(vault, "pull", "--rebase", "--autostash", "-q").returncode == 0:
            p = _git(vault, "push", "-q")
    r["pushed"] = p.returncode == 0
    if not r["pushed"]:
        r["push_error"] = (p.stderr or p.stdout).strip()[-300:] or "push failed"
    return r


def _save(vault, data, rels_extra, message):
    _file(vault).write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n")
    return _commit(vault, [PROJECTS_REL] + rels_extra, message)


def _allow(vault, repos):
    p = Path(vault) / ALLOWLIST
    text = p.read_text() if p.exists() else ""
    have = {l.strip() for l in text.splitlines() if l.strip() and not l.startswith("#")}
    new = [r for r in repos if r not in have]
    if new:
        p.write_text(text + ("" if text.endswith("\n") or not text else "\n") + "".join(r + "\n" for r in new))
    return [ALLOWLIST] if new else []


def _clean(spec):
    out = {"name": str(spec.get("name") or "").strip()}
    for k in ("notes", "folders", "repos", "concept"):
        out[k] = [str(x).strip().rstrip("/") if k == "folders" else str(x).strip()
                  for x in spec.get(k) or [] if str(x).strip()]
    return out


def add(vault, spec, roots):
    data = read_file(vault)
    c = check(spec, vault, roots, existing=data["projects"])
    if not spec.get("id"):
        c["errors"].insert(0, "an id is needed")
    if c["errors"]:
        raise ProjectsError("; ".join(c["errors"]), c["errors"])
    cfg = dict(_clean(spec), active=True)
    data["projects"][spec["id"]] = cfg
    extra = _allow(vault, cfg["repos"])
    r = _save(vault, data, extra, "Radio: add project %s" % spec["id"])
    return dict(r, project=spec["id"])


def update(vault, pid, fields, roots):
    data = read_file(vault)
    if pid not in data["projects"]:
        raise KeyError(pid)
    fields = {k: v for k, v in fields.items() if k in FIELDS}
    c = check(fields, vault, roots)
    if c["errors"]:
        raise ProjectsError("; ".join(c["errors"]), c["errors"])
    cfg = data["projects"][pid]
    cfg.update({k: v for k, v in _clean(dict(cfg, **fields)).items() if k in fields})
    extra = _allow(vault, cfg.get("repos", [])) if "repos" in fields else []
    return dict(_save(vault, data, extra, "Radio: edit project %s" % pid), project=pid)


def set_active(vault, pid, active):
    data = read_file(vault)
    if pid not in data["projects"]:
        raise KeyError(pid)
    data["projects"][pid]["active"] = bool(active)
    return dict(_save(vault, data, [], "Radio: %s %s" % ("activate" if active else "deactivate", pid)), project=pid)
