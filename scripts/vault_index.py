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
  mentions (src doc, kind, root, value) — what a note names outside the vault:
         path  ~/nonvis/<repo>/<file> or ~/projects/… (root = ~/<area>/<name>)
         file  a backticked repo-relative file (`scripts/run.py`), repo unknown
         sha   a backticked commit hash (7–40 hex, a digit and a letter)
         pr    "PR #12" / "PR 12" (a bare #12 is a decision or issue, not kept)
  repos  each mentioned root (and root/<first segment>, for a parent dir that
         holds a repo) resolved once by git: repo · not-git · absent, the main
         worktree it belongs to, and origin's URL. The allowlist is the tracked
         file <vault>/06_Metadata/quorum-repos.txt (one repo name per line).
"""
import subprocess
import os
import re
import sqlite3
from pathlib import Path

SCHEMA_VERSION = "2"
ALLOWLIST = "06_Metadata/quorum-repos.txt"
M_PATH = re.compile(r"(?:~|/Users/[^/\s`]+)/((?:nonvis|projects)/[A-Za-z0-9._-]+)"
                    r"((?:/[A-Za-z0-9._@+-]+)*)")
M_FILE = re.compile(r"`((?:[\w.-]+/)*[\w-][\w.-]*\.(?:py|rs|ts|tsx|js|mjs|move|cpp|cc|h|"
                    r"hpp|sh|toml|json|ya?ml|sql|go|md))(?::\d+)?`")
M_SHA = re.compile(r"`([0-9a-f]{7,40})`")
M_PR = re.compile(r"\bPR\s*#?(\d{1,5})\b")
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
    return ("/Archive/" in path or "/archive/" in path or ":04_Archive/" in path
            or path.startswith("memory:archive/"))


def query_terms(text):
    terms = []
    for tok in re.findall(r"\w+", text.lower(), re.UNICODE):
        if tok in STOPWORDS or (len(tok) < 2 and tok.isascii()):
            continue
        if tok not in terms:
            terms.append(tok)
    return terms


def extract_mentions(body):
    """-> [(kind, root, value)], deduplicated, in order."""
    out = []
    for m in M_PATH.finditer(body):
        root = "~/" + m.group(1).rstrip(".")
        out.append(("path", root, m.group(2).rstrip(".").lstrip("/")))
    for m in M_FILE.finditer(body):
        out.append(("file", "", m.group(1)))
    for m in M_SHA.finditer(body):
        v = m.group(1)
        if re.search(r"[0-9]", v) and re.search(r"[a-f]", v):
            out.append(("sha", "", v))
    for m in M_PR.finditer(body):
        out.append(("pr", "", m.group(1)))
    seen, uniq = set(), []
    for t in out:
        if t not in seen:
            seen.add(t)
            uniq.append(t)
    return uniq


def _git(path, *args):
    r = subprocess.run(["git", "-C", path] + list(args), capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def _tilde(path):
    home = os.path.expanduser("~")
    return "~" + path[len(home):] if path.startswith(home + "/") else path


def resolve_root(root):
    """-> (status, canonical main-worktree path in ~ form, remote)."""
    full = os.path.expanduser(root)
    if not os.path.exists(full):
        return "absent", None, None
    top = _git(full, "rev-parse", "--show-toplevel")
    if not top:
        return "not-git", None, None
    common = _git(full, "rev-parse", "--path-format=absolute", "--git-common-dir")
    main = os.path.dirname(common) if common and common.endswith("/.git") else top
    return "repo", _tilde(os.path.realpath(main)), _git(main, "config", "remote.origin.url")


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
            for t in ("docs", "fts", "links", "mentions", "repos"):
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
            CREATE TABLE IF NOT EXISTS mentions(src INTEGER, kind TEXT, root TEXT, value TEXT);
            CREATE INDEX IF NOT EXISTS mentions_src ON mentions(src);
            CREATE INDEX IF NOT EXISTS mentions_root ON mentions(root);
            CREATE TABLE IF NOT EXISTS repos(root TEXT PRIMARY KEY, status TEXT,
                canonical TEXT, remote TEXT);
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
            for t in ("docs", "fts", "links", "mentions", "repos"):
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
        if changed or removed or rebuild:
            self._resolve_new_roots()
        c.commit()
        return {"changed": changed, "removed": removed, "total": len(seen)}

    def _delete(self, doc_id):
        self.db.execute("DELETE FROM docs WHERE id=?", (doc_id,))
        self.db.execute("DELETE FROM fts WHERE rowid=?", (doc_id,))
        self.db.execute("DELETE FROM links WHERE src=?", (doc_id,))
        self.db.execute("DELETE FROM mentions WHERE src=?", (doc_id,))

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
        self.db.executemany("INSERT INTO mentions(src, kind, root, value) VALUES(?,?,?,?)",
                            [(doc_id,) + m for m in extract_mentions(body)])

    def _resolve_new_roots(self):
        """git-resolve each root (and root/<first segment>) not seen before."""
        have = {r[0] for r in self.db.execute("SELECT root FROM repos")}
        want = set()
        for r in self.db.execute("SELECT DISTINCT root, value FROM mentions WHERE kind='path'"):
            want.add(r["root"])
            seg = r["value"].split("/", 1)[0]
            if seg:
                want.add(r["root"] + "/" + seg)
        for root in sorted(want - have):
            if root.count("/") > 2 and self._status(root.rsplit("/", 1)[0]) == "repo":
                continue  # inside a repo: the root already answers
            self.db.execute("INSERT OR REPLACE INTO repos VALUES(?,?,?,?)",
                            (root,) + resolve_root(root))

    def _status(self, root):
        r = self.db.execute("SELECT status FROM repos WHERE root=?", (root,)).fetchone()
        if r is None:
            st = resolve_root(root)
            self.db.execute("INSERT OR REPLACE INTO repos VALUES(?,?,?,?)", (root,) + st)
            return st[0]
        return r[0]

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

    def salient_terms(self, text, max_df=0.15, limit=10):
        """The prompt's terms that are RARE in the index (in at most max_df of
        docs, at least one), rarest first — so a common word never triggers."""
        n = self.db.execute("SELECT count(*) FROM docs").fetchone()[0]
        if not n:
            return []
        scored = []
        for t in query_terms(text):
            if len(t) < 3 or t.isdigit():
                continue
            df = self.db.execute("SELECT count(*) FROM fts WHERE fts MATCH ?",
                                 ('"%s"*' % t,)).fetchone()[0]
            if 0 < df <= max(1, max_df * n):
                scored.append((df, t))
        return [t for _, t in sorted(scored)[:limit]]

    def prompt_hits(self, prompt, limit=5):
        """Notes clearly relevant to a prompt, for the UserPromptSubmit hook:
        >= 2 salient terms in the prompt; a hit covers >= 2 of them and at least
        a third, and names one in its title, summary or path — two when the prompt
        has <= 3 salient terms (a note ABOUT the topic says so there; body-only
        or single-word head matches are coincidence). -> (terms, hits)."""
        terms = self.salient_terms(prompt)
        if len(terms) < 2:
            return terms, []
        need = max(2, -(-len(terms) // 3))
        pats = [re.compile(r"(?<!\w)" + re.escape(t)) for t in terms]
        need_head = 2 if len(terms) <= 3 else 1
        hits = []
        for h in self.search(" ".join(terms), limit=limit * 4):
            head = ("%s %s %s" % (h["path"], h["title"], h["summary"][:300])).lower()
            if h["matched"] >= need and sum(1 for p in pats if p.search(head)) >= need_head:
                hits.append(h)
        return terms, hits[:limit]

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


    # -- step 2: mentions + repos ---------------------------------------------
    def _allowlist(self):
        p = self.vault / ALLOWLIST
        if not p.exists():
            return set()
        return {l.split("#", 1)[0].strip() for l in p.read_text().splitlines()} - {""}

    def _repo_of(self, root, value):
        """The canonical repo a path mention belongs to, or (status, name)."""
        rows = {r["root"]: r for r in self.db.execute(
            "SELECT * FROM repos WHERE root IN (?, ?)",
            (root, root + "/" + value.split("/", 1)[0]))}
        top = rows.get(root)
        if top and top["status"] == "repo":
            return top
        sub = rows.get(root + "/" + value.split("/", 1)[0]) if value else None
        if sub and sub["status"] == "repo":
            return sub
        return top

    def repos(self):
        allow = self._allowlist()
        out = {}
        for m in self.db.execute("SELECT src, root, value FROM mentions WHERE kind='path'"):
            r = self._repo_of(m["root"], m["value"])
            if r is None:
                continue
            key = r["canonical"] if r["status"] == "repo" else r["root"]
            e = out.setdefault(key, {
                "name": os.path.basename(key), "status": r["status"],
                "path": key, "remote": r["remote"], "mentions": 0, "srcs": set()})
            e["mentions"] += 1
            e["srcs"].add(m["src"])
        res = []
        for e in out.values():
            e["notes"] = len(e.pop("srcs"))
            e["allowed"] = e["status"] == "repo" and (e["name"] in allow or e["path"] in allow)
            res.append(e)
        return sorted(res, key=lambda e: (e["status"] != "repo", -e["notes"], e["name"]))

    def _sha_repo(self, sha):
        for r in self.db.execute("SELECT DISTINCT canonical FROM repos WHERE status='repo'"):
            if _git(os.path.expanduser(r[0]), "cat-file", "-e", sha + "^{commit}") is not None:
                return os.path.basename(r[0])
        return None

    def mentions(self, term):
        """Notes naming a repo (by name or ~ path), a file, or a commit."""
        term = term.strip()
        hits = {}

        def add(src, kind, value, repo=None):
            d = self.db.execute("SELECT path, title FROM docs WHERE id=?", (src,)).fetchone()
            if d:
                hits.setdefault(d["path"], {"path": d["path"], "title": d["title"],
                                            "kind": kind, "value": value, "repo": repo})

        if re.fullmatch(r"[0-9a-f]{7,40}", term):
            for m in self.db.execute("SELECT src, value FROM mentions WHERE kind='sha'"):
                if term.startswith(m["value"]) or m["value"].startswith(term):
                    add(m["src"], "sha", m["value"], self._sha_repo(m["value"]))
            return sorted(hits.values(), key=lambda h: h["path"])
        names = {e["name"]: e["path"] for e in self.repos()}
        repo_path = names.get(term) or (term if term.startswith("~/") else None)
        for m in self.db.execute("SELECT src, kind, root, value FROM mentions"
                                 " WHERE kind IN ('path', 'file')"):
            if m["kind"] == "path":
                r = self._repo_of(m["root"], m["value"])
                key = (r["canonical"] if r and r["status"] == "repo" else m["root"])
                full = m["root"] + ("/" + m["value"] if m["value"] else "")
                if repo_path and (key == repo_path or m["root"] == repo_path):
                    add(m["src"], "path", full, os.path.basename(key))
                elif not repo_path and full.endswith("/" + term):
                    add(m["src"], "path", full, os.path.basename(key))
            elif not repo_path and (m["value"] == term or m["value"].endswith("/" + term)
                                    or term.endswith("/" + m["value"])):
                add(m["src"], "file", m["value"])
        return sorted(hits.values(), key=lambda h: h["path"])

    def note_mentions(self, name):
        d = self._resolve(name)
        if d is None:
            return None
        return [dict(r) for r in self.db.execute(
            "SELECT kind, root, value FROM mentions WHERE src=?", (d["id"],))]


def open_default():
    vault = default_vault()
    return Index(vault / ".quorum" / "index.db", vault, default_memory(vault))
