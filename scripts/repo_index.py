#!/usr/bin/env python3
"""The per-repo index (step 3) — <repo>/.quorum/index.db, stdlib only, no model calls.

It indexes the repo's COMMITTED tree at HEAD (uncommitted edits are not in it):

  files    every tracked blob: path, blob sha, kind (doc | code | other |
           vendored — under third_party/, vendor/, node_modules/, dist/, build/:
           listed, never parsed)
  docs     markdown full text (FTS5: path · title · body); docs/** and the root
           guides (README, ARCHITECTURE, OPERATOR, CLAUDE, …) rank first
  symbols  definitions found by per-language patterns (Python, Rust, TS/JS,
           Move, C/C++, Go, shell): name, kind, path, line
  commits  every commit on HEAD's history (FTS5: subject · body)

Every read refreshes first: `git ls-tree` against the stored blob shas re-reads
only the blobs that changed, and `git log <indexed head>..HEAD` adds the new
commits (a history that no longer contains the indexed head is re-read whole).
The repo stays clean: `.quorum/.gitignore` holds `*` (written only if missing).
"""
import os
import re
import sqlite3
import subprocess
from pathlib import Path

from vault_index import fts_query, query_terms

SCHEMA_VERSION = "2"
MAX_BLOB = 1_000_000
DOC_EXT = {".md", ".mdx"}
VENDORED = {"third_party", "vendor", "vendored", "node_modules", "dist", "build", ".venv"}


ROOT_GUIDES = {"readme.md", "architecture.md", "operator.md", "claude.md", "contributing.md",
               "design.md", "security.md"}


def is_docs(path):
    """docs/** and the root guides — the repo's locked, self-contained record."""
    p = path.lower()
    return p.startswith("docs/") or ("/" not in p and p in ROOT_GUIDES)


def file_kind(path):
    if VENDORED & set(path.split("/")[:-1]):
        return "vendored"
    ext = os.path.splitext(path)[1].lower()
    return "doc" if ext in DOC_EXT else "code" if ext in LANG else "other"

_TS = re.compile(r"^(\s*)(?:export\s+)?(?:default\s+)?(?:declare\s+)?(?:abstract\s+)?(?:async\s+)?"
                 r"(function\*?|class|interface|type|enum|const|let)\s+([A-Za-z_$][\w$]*)")
_CPP_FN = re.compile(r"^(?!\s)(?!(?:if|for|while|switch|return|else|do|case|using|typedef)\b)"
                     r"[\w:<>,\s\*&~]*?\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:const\s*)?"
                     r"(?:noexcept\s*)?(?:override\s*)?(?:->\s*[\w:<>]+\s*)?\{?\s*$")
PATTERNS = {
    "py": [(re.compile(r"^\s*(class)\s+(\w+)"), None),
           (re.compile(r"^\s*(?:async\s+)?(def)\s+(\w+)"), None)],
    "rs": [(re.compile(r"^\s*(?:pub(?:\([^)]*\))?\s+)?(?:(?:async|const|unsafe|extern)\s+)*"
                       r"(fn|struct|enum|trait|mod|type|const|static|union)\s+(\w+)"), None)],
    "move": [(re.compile(r"^\s*(module)\s+(?:[\w]+::)?(\w+)"), None),
             (re.compile(r"^\s*(?:public(?:\([\w]+\))?\s+)?(?:entry\s+)?(?:native\s+)?(?:macro\s+)?"
                         r"(fun)\s+(\w+)"), None),
             (re.compile(r"^\s*(?:public\s+)?(struct|enum)\s+(\w+)"), None),
             (re.compile(r"^\s*(const)\s+(\w+)"), None)],
    "cpp": [(re.compile(r"^\s*(namespace)\s+(\w+)"), None),
            (re.compile(r"^\s*(?:template\s*<[^>]*>\s*)?(class|struct|enum class|enum)\s+(\w+)"
                        r"\s*(?:final\s*)?(?:[:{]|$)"), None),
            (_CPP_FN, "fn")],
    "go": [(re.compile(r"^(func)\s+(?:\([^)]*\)\s*)?(\w+)"), None),
           (re.compile(r"^(type)\s+(\w+)"), None)],
    "sh": [(re.compile(r"^\s*(?:(function)\s+)?([A-Za-z_][\w-]*)\s*\(\)\s*\{"), "fn")],
}
LANG = {".py": "py", ".rs": "rs", ".move": "move", ".go": "go", ".sh": "sh", ".bash": "sh",
        ".c": "cpp", ".cc": "cpp", ".cpp": "cpp", ".cxx": "cpp", ".h": "cpp", ".hh": "cpp",
        ".hpp": "cpp", ".ts": "ts", ".tsx": "ts", ".mts": "ts", ".cts": "ts", ".js": "ts",
        ".jsx": "ts", ".mjs": "ts", ".cjs": "ts"}


def symbols_of(path, text):
    """-> [(name, kind, line)] for one file's text."""
    lang = LANG.get(os.path.splitext(path)[1].lower())
    if not lang:
        return []
    out = []
    for n, line in enumerate(text.split("\n"), 1):
        if len(line) > 400:
            continue
        if lang == "ts":
            m = _TS.match(line)
            if m and not (m.group(2) in ("const", "let") and m.group(1)):
                out.append((m.group(3), m.group(2).rstrip("*"), n))
            continue
        for pat, kind in PATTERNS[lang]:
            m = pat.match(line)
            if m:
                if kind == "fn" and pat is _CPP_FN:
                    out.append((m.group(1), "fn", n))
                else:
                    out.append((m.group(2), kind or m.group(1), n))
                break
    return out


def git(repo, *args, input=None):
    r = subprocess.run(["git", "-C", str(repo)] + list(args), capture_output=True,
                       input=input)
    return r.stdout if r.returncode == 0 else None


def toplevel(path):
    out = git(path, "rev-parse", "--show-toplevel")
    return Path(out.decode().strip()) if out else None


class RepoIndex:
    def __init__(self, repo):
        self.repo = Path(repo)
        qdir = self.repo / ".quorum"
        qdir.mkdir(exist_ok=True)
        gi = qdir / ".gitignore"
        if not gi.exists():
            gi.write_text("*\n")
        self.db = sqlite3.connect(str(qdir / "index.db"))
        self.db.row_factory = sqlite3.Row
        self._schema()

    def close(self):
        self.db.close()

    def _schema(self):
        c = self.db
        c.execute("CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT)")
        row = c.execute("SELECT value FROM meta WHERE key='schema'").fetchone()
        if row is None or row[0] != SCHEMA_VERSION:
            for t in ("files", "docs", "symbols", "commits", "commits_fts"):
                c.execute("DROP TABLE IF EXISTS %s" % t)
            c.execute("DELETE FROM meta")
        c.executescript("""
            CREATE TABLE IF NOT EXISTS files(id INTEGER PRIMARY KEY, path TEXT UNIQUE,
                blob TEXT, kind TEXT);
            CREATE VIRTUAL TABLE IF NOT EXISTS docs USING fts5(path, title, body,
                tokenize='unicode61 remove_diacritics 2');
            CREATE TABLE IF NOT EXISTS symbols(file INTEGER, name TEXT, kind TEXT, line INTEGER);
            CREATE INDEX IF NOT EXISTS symbols_name ON symbols(name COLLATE NOCASE);
            CREATE INDEX IF NOT EXISTS symbols_file ON symbols(file);
            CREATE TABLE IF NOT EXISTS commits(sha TEXT PRIMARY KEY, date TEXT, author TEXT,
                subject TEXT);
            CREATE VIRTUAL TABLE IF NOT EXISTS commits_fts USING fts5(sha UNINDEXED, subject,
                body, tokenize='unicode61 remove_diacritics 2');
        """)
        c.execute("INSERT OR REPLACE INTO meta VALUES('schema', ?)", (SCHEMA_VERSION,))
        c.commit()

    def _meta(self, key):
        r = self.db.execute("SELECT value FROM meta WHERE key=?", (key,)).fetchone()
        return r[0] if r else None

    # -- refresh --------------------------------------------------------------
    def refresh(self, rebuild=False):
        c = self.db
        if rebuild:
            for t in ("files", "docs", "symbols", "commits", "commits_fts"):
                c.execute("DELETE FROM %s" % t)
            c.execute("DELETE FROM meta WHERE key='head'")
        head = git(self.repo, "rev-parse", "--verify", "-q", "HEAD")
        head = head.decode().strip() if head else ""
        if head and head == self._meta("head"):
            return {"files": 0, "commits": 0, "head": head}
        nfiles = self._refresh_files() if head else 0
        ncommits = self._refresh_commits(head) if head else 0
        c.execute("INSERT OR REPLACE INTO meta VALUES('head', ?)", (head,))
        c.commit()
        return {"files": nfiles, "commits": ncommits, "head": head}

    def _refresh_files(self):
        tree = {}
        for rec in (git(self.repo, "ls-tree", "-r", "-z", "--long", "HEAD") or b"").split(b"\0"):
            if not rec:
                continue
            meta, path = rec.split(b"\t", 1)
            mode, typ, blob, size = meta.split()
            if typ == b"blob":
                tree[path.decode("utf-8", "replace")] = (blob.decode(), size.decode())
        known = {r["path"]: (r["id"], r["blob"]) for r in
                 self.db.execute("SELECT id, path, blob FROM files")}
        for path, (fid, _) in known.items():
            if path not in tree:
                self._drop(fid)
        todo = [(p, b, s) for p, (b, s) in tree.items()
                if p not in known or known[p][1] != b]
        for path, _, _ in todo:
            if path in known:
                self._drop(known[path][0])
        wanted = [(p, b) for p, b, s in todo if s != "-" and int(s) <= MAX_BLOB
                  and file_kind(p) in ("doc", "code")]
        texts = self._cat(b for _, b in wanted)
        for path, blob, _ in todo:
            kind = file_kind(path)
            fid = self.db.execute("INSERT INTO files(path, blob, kind) VALUES(?,?,?)",
                                  (path, blob, kind)).lastrowid
            text = texts.get(blob)
            if text is None or "\0" in text[:8000]:
                continue
            if kind == "doc":
                m = re.search(r"^title:\s*(.+)$", text[:2000], re.M) or \
                    re.search(r"^#\s+(.+)$", text, re.M)
                title = m.group(1).strip().strip("\"'") if m else os.path.basename(path)
                self.db.execute("INSERT INTO docs(rowid, path, title, body) VALUES(?,?,?,?)",
                                (fid, path, title, text))
            elif kind == "code":
                self.db.executemany(
                    "INSERT INTO symbols(file, name, kind, line) VALUES(?,?,?,?)",
                    [(fid,) + s for s in symbols_of(path, text)])
        return len(todo)

    def _cat(self, blobs):
        blobs = list(dict.fromkeys(blobs))
        if not blobs:
            return {}
        out = git(self.repo, "cat-file", "--batch", input=("\n".join(blobs) + "\n").encode())
        texts, i = {}, 0
        while out and i < len(out):
            nl = out.index(b"\n", i)
            header = out[i:nl].split()
            if len(header) < 3:
                i = nl + 1
                continue
            sha, size = header[0].decode(), int(header[2])
            texts[sha] = out[nl + 1: nl + 1 + size].decode("utf-8", "replace")
            i = nl + 1 + size + 1
        return texts

    def _drop(self, fid):
        self.db.execute("DELETE FROM files WHERE id=?", (fid,))
        self.db.execute("DELETE FROM docs WHERE rowid=?", (fid,))
        self.db.execute("DELETE FROM symbols WHERE file=?", (fid,))

    def _refresh_commits(self, head):
        old = self._meta("head")
        rng = [head]
        if old and git(self.repo, "merge-base", "--is-ancestor", old, head) is not None:
            rng = ["%s..%s" % (old, head)]
        else:
            self.db.execute("DELETE FROM commits")
            self.db.execute("DELETE FROM commits_fts")
        out = git(self.repo, "log", "--format=%H%x1f%aI%x1f%an%x1f%s%x1f%b%x1e", *rng) or b""
        n = 0
        for rec in out.decode("utf-8", "replace").split("\x1e"):
            parts = rec.strip("\n").split("\x1f")
            if len(parts) < 5:
                continue
            sha, date, author, subject, body = parts
            cur = self.db.execute("INSERT OR IGNORE INTO commits VALUES(?,?,?,?)",
                                  (sha, date, author, subject))
            if cur.rowcount:
                self.db.execute("INSERT INTO commits_fts(sha, subject, body) VALUES(?,?,?)",
                                (sha, subject, body))
                n += 1
        return n

    # -- reads ----------------------------------------------------------------
    def stats(self):
        one = lambda q: self.db.execute(q).fetchone()[0]  # noqa: E731
        return {"files": one("SELECT count(*) FROM files"),
                "docs": one("SELECT count(*) FROM files WHERE kind='doc'"),
                "symbols": one("SELECT count(*) FROM symbols"),
                "commits": one("SELECT count(*) FROM commits"),
                "head": (self._meta("head") or "")[:12]}

    def symbol(self, name, limit=50):
        rows = self.db.execute(
            "SELECT s.name, s.kind, s.line, f.path FROM symbols s JOIN files f ON f.id = s.file"
            " WHERE s.name LIKE ? ESCAPE '\\' ORDER BY s.name = ? COLLATE NOCASE DESC,"
            " length(s.name), f.path, s.line LIMIT ?",
            (name.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_") + "%",
             name, limit))
        return [dict(r) for r in rows]

    def search(self, text, limit=10):
        terms = query_terms(text)
        if not terms:
            return []
        q = fts_query(terms)
        pats = [re.compile(r"(?<!\w)" + re.escape(t)) for t in terms]
        hits = []
        for r in self.db.execute(
                "SELECT path, title, bm25(docs, 1.0, 10.0, 1.0) AS score,"
                " snippet(docs, 2, '[', ']', '…', 12) AS snip, lower(path||' '||title||' '||body) t"
                " FROM docs WHERE docs MATCH ? ORDER BY score LIMIT 100", (q,)):
            hits.append({"kind": "doc", "ref": r["path"], "title": r["title"],
                         "snippet": r["snip"], "score": r["score"], "docs": is_docs(r["path"]),
                         "matched": sum(1 for p in pats if p.search(r["t"]))})
        for r in self.db.execute(
                "SELECT f.sha, f.subject, c.date, bm25(commits_fts, 0, 5.0, 1.0) AS score,"
                " snippet(commits_fts, 2, '[', ']', '…', 12) AS snip,"
                " lower(f.subject||' '||f.body) t"
                " FROM commits_fts f JOIN commits c ON c.sha = f.sha WHERE commits_fts MATCH ?"
                " ORDER BY score LIMIT 100", (q,)):
            hits.append({"kind": "commit", "ref": r["sha"][:10], "title": r["subject"],
                         "snippet": r["snip"] or r["date"][:10], "score": r["score"], "docs": False,
                         "matched": sum(1 for p in pats if p.search(r["t"]))})
        for t in terms:
            for s in self.symbol(t, limit=20):
                exact = s["name"].lower() == t
                hits.append({"kind": "symbol", "ref": "%s:%d" % (s["path"], s["line"]),
                             "title": "%s %s" % (s["kind"], s["name"]), "snippet": "",
                             "score": -1000.0 if exact else -10.0, "matched": 1, "docs": False})
        seen, out = set(), []
        for h in sorted(hits, key=lambda h: (-h["matched"], not h["docs"], h["score"])):
            if (h["kind"], h["ref"]) not in seen:
                seen.add((h["kind"], h["ref"]))
                out.append(h)
        return out[:limit]
