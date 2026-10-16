#!/usr/bin/env python3
"""The vault index — SQLite FTS5 over a vault's notes and its Claude Code memory
files, plus their [[wikilinks]]. Deterministic, stdlib only, no model calls.

The database lives at <vault>/.quorum/index.db (never tracked). Every query
first runs refresh(), which stats each markdown file and re-parses only the ones
whose mtime or size changed, and drops the ones that are gone — so the index
can't be stale when it is read, and nothing has to be scheduled.

  docs   one row per file: path ("vault:<rel>" | "memory:<rel>"), stem, title,
         summary (frontmatter `summary:` or a memory's `description:`), tags,
         type, updated, mtime, size
  fts    title · summary · tags · body, ranked by bm25 (weights 10 · 5 · 3 · 1)
  links  (src doc, target name) — resolved at read time by file stem, so a new
         note fills a link that was dangling without re-reading its sources
"""
import os
import re
import sqlite3
from pathlib import Path

SCHEMA_VERSION = "1"
SKIP_DIRS = {".git", ".obsidian", "node_modules", ".quorum", ".trash", "__pycache__"}
WIKILINK = re.compile(r"\[\[([^\]\n]+?)\]\]")
STOPWORDS = set("""a an and are as at be by do does for from has have how i in is it
its of on or so that the this to was we what when where which who why will with
you your can should would could about into than then them they there these those
""".split())
BM25 = "bm25(fts, 10.0, 5.0, 3.0, 1.0)"


def default_vault():
    return Path(os.environ.get("QUORUM_VAULT")
                or os.path.expanduser("~/nonvis/second-brain")).resolve()


def default_memory(vault):
    env = os.environ.get("QUORUM_MEMORY")
    if env:
        return Path(env)
    munged = "".join("-" if c in "/." else c for c in str(vault))
    return Path(os.path.expanduser("~/.claude/projects")) / munged / "memory"


def _unquote(v):
    v = v.strip()
    if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
        v = v[1:-1]
    return v


def parse(text):
    """-> (frontmatter dict of top-level + `metadata.type`, body). A small YAML
    subset: `key: value`, `[a, b]` lists, `- item` lists, one nested `type:`."""
    fm, body = {}, text
    if text.startswith("---\n"):
        end = text.find("\n---", 4)
        if end != -1:
            block, body = text[4:end], text[end + 4:].lstrip("-").lstrip("\n")
            key = None
            for line in block.split("\n"):
                if not line.strip():
                    continue
                indented = line[0] in " \t"
                s = line.strip()
                if indented and s.startswith("- ") and key:
                    fm[key] = (fm.get(key, "") + " " + _unquote(s[2:])).strip()
                    continue
                if ":" not in s:
                    continue
                k, v = s.split(":", 1)
                k, v = k.strip(), v.strip()
                if indented:
                    if k == "type" and "type" not in fm:
                        fm["type"] = _unquote(v)
                    continue
                key = k
                if v.startswith("[") and v.endswith("]"):
                    v = " ".join(_unquote(x) for x in v[1:-1].split(",") if x.strip())
                fm[k] = _unquote(v)
    return fm, body


def link_targets(body):
    out = []
    for m in WIKILINK.finditer(body):
        t = m.group(1)
        t = re.split(r"\\?\|", t, 1)[0]
        t = t.split("#", 1)[0].strip()
        if not t:
            continue
        t = t.rstrip("\\").split("/")[-1]
        if t.endswith(".md"):
            t = t[:-3]
        out.append(t)
    return out


def is_archived(path):
    return "/Archive/" in path or ":04_Archive/" in path or "/archive/" in path


def query_terms(text):
    terms = []
    for tok in re.findall(r"\w+", text.lower(), re.UNICODE):
        if tok in STOPWORDS or (len(tok) < 2 and tok.isascii()):
            continue
        if tok not in terms:
            terms.append(tok)
    return terms


def fts_query(terms):
    """Any term matches (OR), each as a prefix."""
    return " OR ".join('"%s"*' % t for t in terms)


class Index:
    def __init__(self, db_path, vault, memory=None):
        self.vault = Path(vault)
        self.memory = Path(memory) if memory else None
        Path(db_path).parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(str(db_path))
        self.db.row_factory = sqlite3.Row
        self._schema()

    def close(self):
        self.db.close()

    def _schema(self):
        c = self.db
        c.execute("CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT)")
        row = c.execute("SELECT value FROM meta WHERE key='schema'").fetchone()
        if row is None or row[0] != SCHEMA_VERSION:
            for t in ("docs", "fts", "links"):
                c.execute("DROP TABLE IF EXISTS %s" % t)
        c.executescript("""
            CREATE TABLE IF NOT EXISTS docs(
                id INTEGER PRIMARY KEY, path TEXT UNIQUE, source TEXT, stem TEXT,
                title TEXT, summary TEXT, tags TEXT, type TEXT, updated TEXT,
                mtime REAL, size INTEGER);
            CREATE INDEX IF NOT EXISTS docs_stem ON docs(stem COLLATE NOCASE);
            CREATE VIRTUAL TABLE IF NOT EXISTS fts USING fts5(
                title, summary, tags, body, tokenize='unicode61 remove_diacritics 2');
            CREATE TABLE IF NOT EXISTS links(src INTEGER, target TEXT);
            CREATE INDEX IF NOT EXISTS links_src ON links(src);
            CREATE INDEX IF NOT EXISTS links_target ON links(target COLLATE NOCASE);
        """)
        c.execute("INSERT OR REPLACE INTO meta VALUES('schema', ?)", (SCHEMA_VERSION,))
        c.commit()

    def _walk(self):
        roots = [("vault", self.vault)]
        if self.memory and self.memory.is_dir():
            roots.append(("memory", self.memory))
        for source, root in roots:
            for dirpath, dirnames, filenames in os.walk(root):
                dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
                for f in filenames:
                    if f.endswith(".md"):
                        full = os.path.join(dirpath, f)
                        rel = os.path.relpath(full, root)
                        yield source, "%s:%s" % (source, rel), full

    def refresh(self, rebuild=False):
        c = self.db
        if rebuild:
            for t in ("docs", "fts", "links"):
                c.execute("DELETE FROM %s" % t)
        known = {r["path"]: (r["id"], r["mtime"], r["size"])
                 for r in c.execute("SELECT id, path, mtime, size FROM docs")}
        seen, changed = set(), 0
        for source, path, full in self._walk():
            seen.add(path)
            try:
                st = os.stat(full)
            except OSError:
                continue
            old = known.get(path)
            if old and old[1] == st.st_mtime and old[2] == st.st_size:
                continue
            if old:
                self._delete(old[0])
            self._insert(source, path, full, st)
            changed += 1
        removed = 0
        for path, (doc_id, _, _) in known.items():
            if path not in seen:
                self._delete(doc_id)
                removed += 1
        c.commit()
        return {"changed": changed, "removed": removed, "total": len(seen)}

    def _delete(self, doc_id):
        self.db.execute("DELETE FROM docs WHERE id=?", (doc_id,))
        self.db.execute("DELETE FROM fts WHERE rowid=?", (doc_id,))
        self.db.execute("DELETE FROM links WHERE src=?", (doc_id,))

    def _insert(self, source, path, full, st):
        try:
            text = Path(full).read_text(encoding="utf-8", errors="replace")
        except OSError:
            return
        fm, body = parse(text)
        stem = Path(full).stem
        title = fm.get("title") or fm.get("name") or stem
        summary = fm.get("summary") or fm.get("description") or ""
        cur = self.db.execute(
            "INSERT INTO docs(path, source, stem, title, summary, tags, type, updated,"
            " mtime, size) VALUES(?,?,?,?,?,?,?,?,?,?)",
            (path, source, stem, title, summary, fm.get("tags", ""), fm.get("type", ""),
             fm.get("updated") or fm.get("date") or "", st.st_mtime, st.st_size))
        doc_id = cur.lastrowid
        self.db.execute("INSERT INTO fts(rowid, title, summary, tags, body) VALUES(?,?,?,?,?)",
                        (doc_id, title + " " + stem, summary, fm.get("tags", ""), body))
        self.db.executemany("INSERT INTO links(src, target) VALUES(?,?)",
                            [(doc_id, t) for t in link_targets(body)])

    # -- reads --------------------------------------------------------------
    def stats(self):
        rows = self.db.execute("SELECT source, count(*) n FROM docs GROUP BY source")
        out = {"vault": 0, "memory": 0}
        out.update({r["source"]: r["n"] for r in rows})
        out["links"] = self.db.execute("SELECT count(*) FROM links").fetchone()[0]
        return out

    def doc(self, path):
        r = self.db.execute("SELECT * FROM docs WHERE path=?", (path,)).fetchone()
        return dict(r) if r else None

    def search(self, text, limit=8, source=None):
        """Ranked by how many query terms a doc covers, then live before archived,
        then bm25 — a short title hit on one term must not outrank a doc that
        matches most of the query."""
        terms = query_terms(text)
        if not terms:
            return []
        sql = ("SELECT d.path, d.title, d.summary, d.updated, %s AS score,"
               " snippet(fts, 3, '[', ']', '…', 12) AS snippet,"
               " lower(fts.title || ' ' || fts.summary || ' ' || fts.tags || ' ' || fts.body)"
               " AS text"
               " FROM fts JOIN docs d ON d.id = fts.rowid WHERE fts MATCH ?" % BM25)
        args = [fts_query(terms)]
        if source:
            sql += " AND d.source = ?"
            args.append(source)
        sql += " ORDER BY score LIMIT ?"
        args.append(max(limit * 10, 50))
        pats = [re.compile(r"(?<!\w)" + re.escape(t)) for t in terms]
        hits = []
        for r in self.db.execute(sql, args):
            h = dict(r)
            text = h.pop("text")
            h["matched"] = sum(1 for p in pats if p.search(text))
            hits.append(h)
        hits.sort(key=lambda h: (-h["matched"], is_archived(h["path"]), h["score"]))
        return hits[:limit]

    def _resolve(self, name):
        """A doc by full path, by path suffix, or by stem (vault first)."""
        r = self.db.execute("SELECT * FROM docs WHERE path=?", (name,)).fetchone()
        if r:
            return r
        stem = Path(name).stem if name.endswith(".md") else name
        return self.db.execute(
            "SELECT * FROM docs WHERE stem = ? COLLATE NOCASE"
            " ORDER BY source = 'memory', length(path) LIMIT 1", (stem,)).fetchone()

    def links(self, name):
        d = self._resolve(name)
        if d is None:
            return None
        out = []
        for r in self.db.execute("SELECT DISTINCT target FROM links WHERE src=?", (d["id"],)):
            t = self.db.execute(
                "SELECT path FROM docs WHERE stem = ? COLLATE NOCASE"
                " ORDER BY source != ?, length(path) LIMIT 1",
                (r["target"], d["source"])).fetchone()
            out.append({"target": r["target"], "path": t["path"] if t else None})
        back = [dict(r) for r in self.db.execute(
            "SELECT DISTINCT s.path, s.title FROM links l JOIN docs s ON s.id = l.src"
            " WHERE l.target = ? COLLATE NOCASE AND s.id != ? ORDER BY s.path",
            (d["stem"], d["id"]))]
        return {"path": d["path"], "title": d["title"], "outgoing": out, "backlinks": back}


def open_default():
    vault = default_vault()
    return Index(vault / ".quorum" / "index.db", vault, default_memory(vault))
