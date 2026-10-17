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
PROJECTS_DIR = "01_Projects"
BRIEFING_NOTE = re.compile(r"^\d{2,3} - Briefing\b.*\(rolling\)\.md$")
STATUS_NOTES = (re.compile(r"^\d{2,3} - .*Index & Status.*\.md$", re.I), re.compile(r"^\d{2,3} - .*\bStatus\b.*\.md$", re.I),
                re.compile(r"^\d{2,3} - .*Current State.*\.md$", re.I))
CONCEPT_NOTE = re.compile(r"^00 - .*\.md$")


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


def _owner(path, existing, key):
    """The name of the project that already holds this repo (key=repos) or folder (key=folders)."""
    for cfg in (existing or {}).values():
        for x in cfg.get(key, []):
            if (key == "repos" and x == path) or (key == "folders" and (
                    x == path or path.startswith(x.rstrip("/") + "/") or x.startswith(path.rstrip("/") + "/"))):
                return cfg.get("name") or "another project"
    return None


def check(spec, vault, roots, existing=None):
    """Each path resolved, for the web's live check: {notes, concept: [{pattern, matches}],
    folders: [{path, exists}], repos: [{name, path, remote, own}], errors: [...]}. The rules:
    folders under 01_Projects/, notes inside the project's own folders, and no repo or
    folder shared with another project (`existing` = the other projects)."""
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
    folders_ok = True
    for f in spec.get("folders") or []:
        ok = _safe(f) and (Path(vault) / f).is_dir()
        out["folders"].append({"path": f, "exists": ok})
        if not ok:
            folders_ok = False
            errs.append("folders: %r is not a folder in the vault" % f)
        elif not f.startswith(PROJECTS_DIR + "/"):
            folders_ok = False
            errs.append("folders: %r is not under %s/" % (f, PROJECTS_DIR))
        elif _owner(f, existing, "folders"):
            errs.append("folders: %r is already in %s" % (f, _owner(f, existing, "folders")))
    if folders_ok and spec.get("folders"):
        roots_ = [f.rstrip("/") + "/" for f in spec["folders"]]
        for key in ("notes", "concept"):
            for pat in spec.get(key) or []:
                if _safe(pat) and not any(pat.startswith(r) for r in roots_):
                    errs.append("%s: %r is outside its folders" % (key, pat))
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
            elif _owner(name, existing, "repos"):
                errs.append("repos: %r is already in %s" % (name, _owner(name, existing, "repos")))
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
    others = {k: v for k, v in data["projects"].items() if k != pid}
    c = check(_clean(dict(data["projects"][pid], **fields)), vault, roots, existing=others)
    # An edit is judged on what it changes: a repo not cloned on this host (#103) never blocks another field.
    touched = set(fields) | ({"notes", "concept"} if "folders" in fields else set())
    c["errors"] = [e for e in c["errors"] if e.split(":", 1)[0] in touched]
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


# ── the streamlined add: the repo is the one input ──────────────────────────

def _project_dirs(vault):
    """The project folders: each folder directly under 01_Projects/, and under 01_Projects/Partners/ each partner."""
    base = Path(vault) / PROJECTS_DIR
    out = []
    for d in sorted(base.iterdir()) if base.is_dir() else []:
        if not d.is_dir() or d.name.startswith(".") or d.name == "Archive":
            continue
        if d.name == "Partners":
            out += [str(x.relative_to(vault)) for x in sorted(d.iterdir()) if x.is_dir() and not x.name.startswith(".")]
        else:
            out.append(str(d.relative_to(vault)))
    return out


def _texts(vault):
    """(note path, text) for every note under 01_Projects/, read once per call."""
    return [(str(f.relative_to(vault)), f.read_text(errors="replace")) for f in (Path(vault) / PROJECTS_DIR).rglob("*.md")]


def mentions(vault, repo, texts=None, dirs=None):
    """{project folder: notes under it that name the repo}, most first."""
    rx = re.compile(r"(?<![\w-])%s(?![\w-])" % re.escape(repo))
    dirs = dirs if dirs is not None else _project_dirs(vault)
    counts = {}
    for rel, text in (texts if texts is not None else _texts(vault)):
        owner = next((d for d in dirs if rel.startswith(d + "/")), None)
        if owner and repo in text and rx.search(text):
            counts[owner] = counts.get(owner, 0) + 1
    return dict(sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])))


def _pattern(vault, path):
    rel = str(path.relative_to(vault))
    d, name = rel.rsplit("/", 1)
    return "%s/%s - *.md" % (d, name.split(" - ", 1)[0])


def derive(vault, roots, repo=None, folder=None):
    """A whole project from one repo (or, vault-only, one folder): the folder whose notes
    name the repo most; its rolling briefing (else its status note, with a warning); its
    `00 - …` note as the concept; name and id from the folder."""
    v = Path(vault)
    warnings, counts = [], {}
    if repo and not folder:
        counts = mentions(v, repo)
        folder = next(iter(counts), None)
        if not folder:
            warnings.append("no project folder under %s/ names %s: pick the folder" % (PROJECTS_DIR, repo))
    spec = {"id": "", "name": "", "notes": [], "folders": [], "repos": [repo] if repo else [], "concept": []}
    if folder:
        d = v / folder
        name = folder.rstrip("/").rsplit("/", 1)[-1]
        spec.update(id=re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-"), name=name, folders=[folder.rstrip("/")])
        files = [f for f in d.rglob("*.md") if "Archive" not in f.relative_to(d).parts and len(f.relative_to(d).parts) <= 2]
        brief = sorted((f for f in files if BRIEFING_NOTE.match(f.name)), key=lambda f: (len(f.relative_to(d).parts), f.name))
        if brief:
            spec["notes"] = [_pattern(v, brief[0])]
        else:
            pick = None
            for rx in STATUS_NOTES:
                hits = sorted((f for f in files if rx.match(f.name)), key=lambda f: -f.stat().st_mtime)
                if hits:
                    pick = hits[0]
                    break
            top = sorted(f for f in d.iterdir() if f.is_file() and CONCEPT_NOTE.match(f.name))
            pick = pick or (top[0] if top else None)
            if pick:
                spec["notes"] = [_pattern(v, pick)]
                warnings.append("no rolling briefing in %s; using %s" % (folder, str(pick.relative_to(v))))
            else:
                warnings.append("no briefing or status note in %s: add one" % folder)
        concept = sorted(f for f in d.iterdir() if f.is_file() and CONCEPT_NOTE.match(f.name))
        if concept and _pattern(v, concept[0]) not in spec["notes"]:
            spec["concept"] = [_pattern(v, concept[0])]
    return {"spec": spec, "warnings": warnings, "mentions": dict(list(counts.items())[:4])}


def candidates(vault, roots):
    """Your own repos (github.com/nonvis) under the roots that no project holds yet, with the folder each would take."""
    taken = {r for cfg in load(vault).values() for r in cfg.get("repos", [])}
    texts, dirs, me = _texts(vault), _project_dirs(vault), Path(vault).resolve()
    out = []
    for root in roots:
        if not Path(root).is_dir():
            continue
        for d in sorted(Path(root).iterdir()):
            if d.name.startswith(".") or d.name in taken or not (d / ".git").exists() or d.resolve() == me:
                continue
            if OWN_REMOTE.search(_remote(d)):
                m = mentions(vault, d.name, texts, dirs)
                out.append({"repo": d.name, "path": str(d), "folder": next(iter(m), None),
                            "notes": next(iter(m.values()), 0)})
                taken.add(d.name)
    return out


def free_folders(vault):
    """Project folders under 01_Projects/ that no project holds (for a vault-only project)."""
    existing = load(vault)
    return [d for d in _project_dirs(vault) if not _owner(d, existing, "folders")]
