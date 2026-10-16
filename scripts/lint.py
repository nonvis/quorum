#!/usr/bin/env python3
"""`quorum lint` — a deterministic hygiene report over the vault index and the
allowlisted repo indexes. It never rewrites a note except with --fix-safe, and
then only links whose intended target is unambiguous.

Checks (each a list in the JSON `checks` object):
  dangling_links       [[target]] matching no note or memory file
  orphans              live vault notes no other note links to (Archive/, Inbox,
                       Action Queue, 06_Metadata and "00 - …" folder roots skipped)
  no_summary           live vault notes without a `summary:` (the Note Map is
                       generated from it)
  dead_mentions        ~/… paths in notes naming a file that is not at HEAD of an
                       allowlisted repo
  memory_unindexed     memory files no MEMORY*.md links to
  memory_index_broken  MEMORY*.md rows linking to a missing file
  docs_vault_refs      repo docs that point into the vault (repos stay
                       self-sufficient: the vault links to docs, never back)
  docs_dead_refs       repo docs naming a repo file that is not at HEAD
  locked_docs_missing  notes whose `locked_in: <repo>/<path>` frontmatter names a
                       doc that is not at HEAD

A file counts as present if it is at HEAD or on disk (a gitignored .env is real).
Memory links resolve by a file's `name:` slug as well as its file name.
Archived sources (Archive/, memory/archive/) and memory files are not linted for
dangling links — memory's own rule is that an unwritten [[name]] marks one worth
writing. A link resolves with dashes and underscores interchangeable.
"""
import os
import re
from pathlib import Path

import repo_index
import vault_index

SKIP_ORPHAN = ("Archive/", "04_Archive/", "00_Inbox/", "Action Queue/", "06_Metadata/",
               "05_Attachments/")
NON_MD = re.compile(r"\.(png|jpe?g|gif|svg|pdf|canvas|base|webp|mp4|mov|csv|xlsx?|pptx?|docx?)$",
                    re.I)
# Checked on prose (code fences and inline code removed): wikilinks that are not
# C++-attribute-shaped, the vault's name, its top-level folders, note-ish phrases.
VAULT_PROSE = re.compile(r"\[\[(?![a-z_]+(?:\([^)]*\))?\]\])[^\]\n]+\]\]|second-brain|"
                         r"\b0[1-6]_(?:Projects|Areas|Resources|Archive|Attachments|Metadata)/|"
                         r"\bvault notes?\b", re.I)
# Checked with inline code kept (cites are often backticked): shorthand note cites
# and private paths outside any repo.
VAULT_CITE = re.compile(r"MVP \(Sui\)/\d|\bOps/\d{2,3}\b|\bRaft/0\d|\bPitch/(?:\d|Broker/|Archive/)|"
                        r"Perf Lab Results/|Action Queue/\d|~/Claude/|\bvault `|"
                        r"\b(?:MVP|QA|Permafrost|Sidebar|Loqua|Meridian|Crucible|Quorum)/\d+\b|"
                        r"\bvault [A-Z][\w ]*/")
MD_LINK = re.compile(r"\]\(([^)#\s:]+\.md)(?:#[^)]*)?\)")
PLACEHOLDER = re.compile(r"YYYY|<[^>]*>|\bpath/to\b|\bpath/file\b|\.\.\.")
FENCE = re.compile(r"```.*?```", re.S)
INLINE = re.compile(r"`[^`\n]*`")
NOT_A_FILE_LINE = re.compile(r"\b(?:retired|deleted|removed|superseded|renamed)\b", re.I)
OUTPUT_EXT = (".json", ".jsonl", ".log", ".csv")
MEM_LINK = re.compile(r"\]\(([^)\s]+\.md)\)")


def _present(files, root, f):
    """At HEAD, as a suffix of a tracked path, on disk, or as an .example/.template twin."""
    if f in files or any(x.endswith("/" + f) for x in files) or (root / f).exists():
        return True
    stem, ext = os.path.splitext(f)
    return any(t in files for t in (f + ".example", f + ".template", stem + ".template" + ext,
                                    stem + ".example" + ext))


def norm(name):
    s = name.lower().replace("—", "-").replace("–", "-")
    return re.sub(r"[\s\-]+", " ", s).strip()


def allowed_repos(idx):
    return {r["name"]: Path(os.path.expanduser(r["path"])) for r in idx.repos() if r["allowed"]}


def _names(db):
    """lowercased link name -> [paths]: every doc's file stem, plus a memory
    file's `name:` slug (memory links are written as slugs)."""
    out = {}
    for r in db.execute("SELECT stem, title, source, path FROM docs"):
        names = {r["stem"].lower()}
        if r["source"] == "memory" and r["title"]:
            names.add(r["title"].lower())
        for n in list(names):
            names.add(n.replace("_", "-"))
            names.add(n.replace("-", "_"))
        for n in names:
            out.setdefault(n, []).append(r["path"])
    return out


def run(idx, fix_safe=False):
    """-> {"checks": {...}, "counts": {...}, "fixed": n}. idx is a refreshed
    vault_index.Index."""
    db = idx.db
    stems = _names(db)
    fixed = 0
    if fix_safe:
        fixed = _fix_safe(idx, stems)
        if fixed:
            idx.refresh()
            stems = _names(db)
    checks = {}

    dangling = []
    for r in db.execute("SELECT DISTINCT d.path, l.target FROM links l JOIN docs d ON d.id = l.src"
                        " ORDER BY d.path, l.target"):
        t = r["target"]
        if vault_index.is_archived(r["path"]) or r["path"].startswith("memory:"):
            continue
        if t.lower() not in stems and not NON_MD.search(t):
            dangling.append({"note": r["path"], "target": t})
    checks["dangling_links"] = dangling

    linked = {r[0].lower() for r in db.execute(
        "SELECT DISTINCT l.target FROM links l JOIN docs s ON s.id = l.src"
        " JOIN docs t ON t.stem = l.target COLLATE NOCASE WHERE t.id != s.id")}
    checks["orphans"] = sorted(
        r["path"] for r in db.execute("SELECT path, stem FROM docs WHERE source='vault'")
        if r["stem"].lower() not in linked and not r["stem"].startswith("00 - ")
        and not any(x in r["path"] for x in SKIP_ORPHAN))
    checks["no_summary"] = sorted(
        r["path"] for r in db.execute("SELECT path FROM docs WHERE source='vault' AND summary=''")
        if not any(x in r["path"] for x in SKIP_ORPHAN) and not vault_index.is_archived(r["path"]))

    repos = allowed_repos(idx)
    rindex = {}

    def files_of(name):
        if name not in rindex:
            ri = repo_index.RepoIndex(repos[name])
            ri.refresh()
            rindex[name] = (ri, {r[0] for r in ri.db.execute("SELECT path FROM files")})
        return rindex[name][1]

    def at_head(name, rel):
        rel = rel[2:] if rel.startswith("./") else rel
        files = files_of(name)
        return (rel in files or any(f.startswith(rel.rstrip("/") + "/") for f in files)
                or (repos[name] / rel).exists())

    dead = []
    for m in db.execute("SELECT d.path note, m.root, m.value FROM mentions m JOIN docs d"
                        " ON d.id = m.src WHERE m.kind='path' ORDER BY d.path"):
        if not m["value"] or "." not in m["value"].rsplit("/", 1)[-1]:
            continue
        row = idx._repo_of(m["root"], m["value"])
        if row is None or row["status"] != "repo":
            continue
        name = os.path.basename(row["canonical"])
        if name not in repos:
            continue
        rel = m["value"] if row["root"] == m["root"] else m["value"].split("/", 1)[-1]
        if not at_head(name, rel):
            dead.append({"note": m["note"], "path": "%s/%s" % (m["root"], m["value"])})
    checks["dead_mentions"] = dead

    unindexed, broken = [], []
    mem = idx.memory
    if mem and mem.is_dir():
        referenced = set()
        for ix in sorted(mem.glob("MEMORY*.md")):
            for target in MEM_LINK.findall(ix.read_text(errors="replace")):
                referenced.add(target)
                if not (mem / target).exists():
                    broken.append({"index": ix.name, "file": target})
        unindexed = sorted(f.name for f in mem.glob("*.md")
                           if not f.name.startswith("MEMORY") and f.name not in referenced)
    checks["memory_unindexed"] = unindexed
    checks["memory_index_broken"] = broken

    vault_refs, doc_dead = [], []
    for name in sorted(repos):
        files_of(name)
        ri = rindex[name][0]
        for r in ri.db.execute("SELECT path, body FROM docs ORDER BY path"):
            if not repo_index.is_docs(r["path"]):
                continue
            nofence = FENCE.sub("", r["body"])
            if VAULT_PROSE.search(INLINE.sub("", nofence)) or VAULT_CITE.search(nofence):
                vault_refs.append({"repo": name, "doc": r["path"]})
            files = rindex[name][1]
            tops = {x.split("/", 1)[0] for x in files if "/" in x}
            seen = set()
            for line in nofence.split("\n"):
                if NOT_A_FILE_LINE.search(line):
                    continue
                refs = [(f, False) for f in vault_index.M_FILE.findall(line)]
                refs += [(f, True) for f in MD_LINK.findall(line)]
                for f, is_link in refs:
                    if (f, is_link) in seen or PLACEHOLDER.search(f):
                        continue
                    seen.add((f, is_link))
                    if is_link or f.startswith("../"):
                        # a markdown link (or ../ path) is relative to the doc
                        f = os.path.normpath(os.path.join(os.path.dirname(r["path"]), f))
                    f = f[2:] if f.startswith("./") else f
                    if f.endswith(OUTPUT_EXT) or f.startswith(".."):
                        continue
                    first = f.split("/", 1)[0]
                    if first in repos and first != name and "/" in f:
                        if _present(files_of(first), repos[first], f.split("/", 1)[1]):
                            continue
                    elif not is_link and ("/" not in f or first not in tops):
                        continue  # a bare name or a path outside this repo's tree
                    if not (_present(files, repos[name], f)
                            or any(_present(files_of(o), repos[o], f) for o in repos if o != name)):
                        doc_dead.append({"repo": name, "doc": r["path"], "file": f})
    checks["docs_vault_refs"] = vault_refs
    checks["docs_dead_refs"] = doc_dead

    locked = []
    for r in db.execute("SELECT path FROM docs WHERE source='vault'"):
        full = idx.vault / r["path"].split(":", 1)[1]
        try:
            head = full.read_text(errors="replace")[:3000]
        except OSError:
            continue
        if "\nlocked_in:" not in head:
            continue
        target = vault_index.parse(head)[0].get("locked_in", "")
        if "/" not in target:
            continue
        name, rel = target.split("/", 1)
        if name not in repos or not at_head(name, rel):
            locked.append({"note": r["path"], "doc": target})
    checks["locked_docs_missing"] = sorted(locked, key=lambda x: x["note"])

    for ri, _ in rindex.values():
        ri.close()
    return {"checks": checks, "counts": {k: len(v) for k, v in checks.items()}, "fixed": fixed}


def _fix_safe(idx, stems):
    """Rewrite [[target]] when target matches no note but exactly one note after
    normalising case, dashes and spaces. Returns the number of links rewritten."""
    by_norm = {}
    for low, paths in stems.items():
        for p in paths:
            by_norm.setdefault(norm(low), set()).add(Path(p.split(":", 1)[1]).stem)
    plan = {}
    for r in idx.db.execute("SELECT DISTINCT d.path, l.target FROM links l JOIN docs d"
                            " ON d.id = l.src"):
        t = r["target"]
        if t.lower() in stems:
            continue
        cands = by_norm.get(norm(t), set())
        if len(cands) == 1:
            plan.setdefault(r["path"], {})[t] = next(iter(cands))
    n = 0
    for path, repl in plan.items():
        src, rel = path.split(":", 1)
        full = (idx.vault if src == "vault" else idx.memory) / rel
        text = full.read_text(encoding="utf-8")
        for old, new in repl.items():
            pat = re.compile(r"(\[\[)" + re.escape(old) + r"(?=[\]|#\\])")
            text, k = pat.subn(lambda m: m.group(1) + new, text)
            n += k
        full.write_text(text, encoding="utf-8")
    return n
