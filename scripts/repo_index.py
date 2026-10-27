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
  occ      every identifier occurrence in the code: name, path, line, column and
           role — def (a symbols row), import (a use/import line), ref (other
           code) or text (inside a comment or string literal). A small lexer per
           language tells code from comments and strings; interpolations
           (`${…}`, f"{…}") are code. Keywords and one-letter names are skipped;
           in comments/strings only identifier-shaped words (snake_case,
           camelCase, a digit) are kept — prose is the docs FTS's job.
  deps     each code file's raw import specifiers (Python, TS/JS, C/C++ includes,
           shell source); `affected` resolves them at query time — see repo_deps.py
  commits  every commit on HEAD's history (FTS5: subject · body)

Every read refreshes first: `git ls-tree` against the stored blob shas re-reads
only the blobs that changed, and `git log <indexed head>..HEAD` adds the new
commits (a history that no longer contains the indexed head is re-read whole).
The repo stays clean: `.quorum/.gitignore` holds `*` (written only if missing).
"""
import bisect
import hashlib
import os
import posixpath
import re
import sqlite3
import subprocess
from pathlib import Path

import repo_deps
from repo_deps import is_test_path  # noqa: F401  (re-exported)
from vault_index import fts_query, query_terms

SCHEMA_VERSION = "4"
# What the rows contain depends on the extraction code, not only the schema: a lexer or
# keyword fix changes occ rows for blobs whose sha did not change. The stored key covers
# both modules' source, so a changed indexer rebuilds every checkout's index on its next
# read (a comment-only edit costs one rebuild, ~12 s on the largest repo).
INDEXER_KEY = hashlib.sha256(b"".join(
    (Path(__file__).resolve().parent / f).read_bytes()
    for f in ("repo_index.py", "repo_deps.py"))).hexdigest()[:12]
TABLES = ("files", "docs", "symbols", "occ", "deps", "commits", "commits_fts")
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


# -- occurrences ---------------------------------------------------------------
KEYWORDS = {lang: set(words.split()) for lang, words in {  # reserved words only, per language:
    # `key` is Move's, not TS's; `map` is Go's, not Rust's — one shared set hid them everywhere
    "rs": """as async await break const continue crate dyn else enum extern false fn for if impl
             in let loop match mod move mut pub ref return self Self static struct super trait
             true type unsafe use where while""",
    "ts": """break case catch class const continue debugger default delete do else enum export
             extends false finally for function if import in instanceof new null return super
             switch this throw true try typeof var void while with yield let static implements
             interface private protected public await async undefined""",
    "py": """False None True and as assert async await break class continue def del elif else
             except finally for from global if import in is lambda nonlocal not or pass raise
             return try while with yield self cls""",
    "go": """break case chan const continue default defer else fallthrough for func go goto if
             import interface map package range return select struct switch type var true false
             nil""",
    "cpp": """auto bool break case catch char class const constexpr continue default delete do
              double else enum explicit extern false float for friend goto if inline int long
              namespace new noexcept nullptr operator private protected public return short
              signed sizeof static struct switch template this throw true try typedef typename
              union unsigned using virtual void volatile while override final include define
              ifdef ifndef endif pragma""",
    "move": """abort acquires as break const continue copy drop else entry enum false friend fun
               has if key let loop match module move mut native public return store struct true
               use while""",
    "sh": "if then else elif fi case esac for while until do done in function select time",
}.items()}

_PY_STR = r"(?:'''[\s\S]*?'''|\"\"\"[\s\S]*?\"\"\"|'(?:\\.|[^'\\\n])*'|\"(?:\\.|[^\"\\\n])*\")"
_LEXERS = {  # the languages without nesting; re.S: `\⏎` is an escape too
    "py": re.compile(r"(?P<c>#[^\n]*)|(?P<t>(?<!\w)(?i:[rb]?f|f[rb])%s)"
                     r"|(?P<s>(?<!\w)(?i:[rbu]|br|rb)?%s)" % (_PY_STR, _PY_STR), re.S),
    "sh": re.compile(r"(?P<h><<-?[ \t]*(?P<q>['\"]?)(?P<tag>\w+)(?P=q)[^\n]*\n.*?\n[ \t]*(?P=tag)\b)"
                     # "…" and \x are code, consumed so a ' or # inside them opens nothing
                     r"|(?P<d>\"(?:\\.|[^\"\\])*\")|(?P<e>\\.)"
                     r"|(?P<c>(?<![\w${])#[^\n]*)|(?P<s>'[^']*')", re.S),
}
_PY_INTERP = re.compile(r"(?<!\{)\{([^{}]*)\}")
_WORD = {"ts": re.compile(r"\d[\w.]*|([A-Za-z_$][\w$]*)"), "": re.compile(r"\d[\w.]*|([A-Za-z_]\w*)")}
_C_NEXT = re.compile(r"[/\"'`{}]")
_C_CHAR = re.compile(r"'(?:\\[^'\n]{1,10}|[^\\'\n])'")
_CPP_RAW = re.compile(r'R"([^()\\\s]{0,16})\(')
_REGEX_AFTER = set("(,=:[!&|?{};+-*%<>~^") | {""}
_SHAPED = re.compile(r"\w_|[a-z0-9][A-Z]|\d")  # prose words in comments/strings are not kept
_IMPORT = re.compile(r"^\s*(?:pub(?:\([^)]*\))?\s+)?(?:use\s|import\b|from\s+\S+\s+import\b"
                     r"|#\s*include\b|extern\s+crate\s|export\s.*\sfrom\s+['\"])")
ROLES = ("def", "import", "ref", "text")


def import_lines(lines):
    """1-based numbers of import lines, a bracketed import's later lines included."""
    out, depth = set(), 0
    for n, line in enumerate(lines, 1):
        if depth > 0 or _IMPORT.match(line):
            out.add(n)
            depth = max(0, depth + line.count("{") + line.count("(")
                        - line.count("}") - line.count(")"))
    return out


def _quoted_end(text, i, q, multiline):
    """Index just past the string opened by text[i] == q; an unescaped newline ends a
    single-line string (error recovery: one bad quote must not swallow the file)."""
    n = len(text)
    j = i + 1
    while j < n:
        c = text[j]
        if c == "\\":
            j += 2
            continue
        if c == q:
            return j + 1
        if c == "\n" and not multiline:
            return j
        j += 1
    return n


def c_family_spans(text, lang):
    """Comment and string spans [(start, end)] of a C-family file (rs, ts, go, cpp, move).
    A scanner, not a regex, because a TS template nests: `a ${b ? `c` : f({d})} e` —
    only the literal parts of a template are spans, its `${…}` is code."""
    ts, spans, n, i = lang == "ts", [], len(text), 0
    stack = []  # ts: "T" = inside a template's literal part, an int = brace depth in its ${…}
    while i < n:
        if stack and stack[-1] == "T":
            j = i
            while j < n and text[j] != "`" and not text.startswith("${", j):
                j += 2 if text[j] == "\\" else 1
            spans.append((i, min(j, n)))
            if j >= n:
                break
            if text[j] == "`":
                stack.pop()
                i = j + 1
            else:
                stack.append(0)
                i = j + 2
            continue
        m = _C_NEXT.search(text, i)
        if not m:
            break
        i, c = m.start(), m.group()
        if c == "/" and text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
        elif c == "/" and text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
        elif c == "/":
            k = i - 1
            while k >= 0 and text[k] in " \t":
                k -= 1
            prev = text[k] if k >= 0 and text[k] != "\n" else ""
            j = _regex_end(text, i) if ts and prev in _REGEX_AFTER else None
            if j is None:
                i += 1
                continue
        elif c == '"':
            h = 0
            while i - h - 1 >= 0 and text[i - h - 1] == "#":
                h += 1
            raw = _CPP_RAW.match(text, i - 1) if lang == "cpp" and i else None
            if lang == "rs" and i - h - 1 >= 0 and text[i - h - 1] == "r" \
                    and not (i - h - 2 >= 0 and (text[i - h - 2].isalnum() and
                                                 text[i - h - 2] != "b" or text[i - h - 2] == "_")):
                end = text.find('"' + "#" * h, i + 1)
                j = n if end < 0 else end + 1 + h
            elif raw:
                end = text.find(")" + raw.group(1) + '"', raw.end())
                j = n if end < 0 else end + len(raw.group(1)) + 2
            else:
                j = _quoted_end(text, i, '"', lang in ("rs", "move"))
        elif c == "'":
            if ts:
                j = _quoted_end(text, i, "'", False)
            else:
                cm = _C_CHAR.match(text, i) if lang in ("rs", "go", "cpp") else None
                if not cm:  # a Rust lifetime, a Move address, a C++14 digit separator
                    i += 1
                    continue
                j = cm.end()
        elif c == "`":
            if ts:
                stack.append("T")
                i += 1
                continue
            if lang != "go":
                i += 1
                continue
            j = text.find("`", i + 1)
            j = n if j < 0 else j + 1
        else:  # { }
            if stack:
                if c == "{":
                    stack[-1] += 1
                elif stack[-1] == 0:
                    stack.pop()
                else:
                    stack[-1] -= 1
            i += 1
            continue
        spans.append((i, j))
        i = j
    return spans


def _regex_end(text, i):
    """Index past a JS regex literal opened at text[i] == "/", or None (a division)."""
    j, cls = i + 1, False
    while j < len(text) and text[j] != "\n":
        c = text[j]
        if c == "\\":
            j += 2
            continue
        if c == "[":
            cls = True
        elif c == "]":
            cls = False
        elif c == "/" and not cls:
            return j + 1
        j += 1
    return None


def _lexer_spans(text, lang):
    spans = []
    for m in _LEXERS[lang].finditer(text):
        if m.lastgroup in ("d", "e"):
            continue
        if m.lastgroup == "t":  # an f-string: its {…} are code
            a = m.start()
            for i in _PY_INTERP.finditer(m.group()):
                spans.append((a, m.start() + i.start(1)))
                a = m.start() + i.end(1)
            spans.append((a, m.end()))
        elif m.lastgroup == "h":  # a heredoc: its opening line stays code
            spans.append((m.start() + m.group().index("\n"), m.end()))
        else:
            spans.append(m.span())
    return spans


def occurrences_of(path, text, defs):
    """-> [(name, line, col, role)] for one file; `defs` = {(name, line)} from symbols_of.
    One row per (name, line, role); col is 1-based."""
    lang = LANG.get(os.path.splitext(path)[1].lower())
    if not lang:
        return []
    starts = [0] + [m.end() for m in re.finditer("\n", text)]
    imports = import_lines(text.split("\n"))
    spans = _lexer_spans(text, lang) if lang in _LEXERS else c_family_spans(text, lang)
    span_starts = [a for a, _ in spans]
    seen, out, keywords = set(), [], KEYWORDS[lang]
    for m in _WORD["ts" if lang == "ts" else ""].finditer(text):
        name, pos = m.group(1), m.start()
        if not name or len(name) < 2 or name in keywords:
            continue
        k = bisect.bisect_right(span_starts, pos) - 1
        code = k < 0 or pos >= spans[k][1]
        if not (code or _SHAPED.search(name)):
            continue
        line = bisect.bisect_right(starts, pos)
        role = ("def" if (name, line) in defs else "import" if line in imports else "ref") \
            if code else "text"
        if (name, line, role) not in seen:
            seen.add((name, line, role))
            out.append((name, line, pos - starts[line - 1] + 1, role))
    return out


def last_part(name):
    """`crate::sweep::run`, `Book.total`, `pkg/mod.Fn` → the bare identifier."""
    parts = [p for p in re.split(r"::|[./#]", name) if p]
    return parts[-1] if parts else name


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
        if row is None or row[0] != "%s:%s" % (SCHEMA_VERSION, INDEXER_KEY):
            for t in TABLES:
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
            CREATE TABLE IF NOT EXISTS occ(file INTEGER, name TEXT, line INTEGER, col INTEGER,
                role TEXT);
            CREATE INDEX IF NOT EXISTS occ_name ON occ(name COLLATE NOCASE);
            CREATE INDEX IF NOT EXISTS occ_file ON occ(file);
            CREATE TABLE IF NOT EXISTS deps(file INTEGER, spec TEXT, kind TEXT);
            CREATE INDEX IF NOT EXISTS deps_file ON deps(file);
            CREATE TABLE IF NOT EXISTS commits(sha TEXT PRIMARY KEY, date TEXT, author TEXT,
                subject TEXT);
            CREATE VIRTUAL TABLE IF NOT EXISTS commits_fts USING fts5(sha UNINDEXED, subject,
                body, tokenize='unicode61 remove_diacritics 2');
        """)
        c.execute("INSERT OR REPLACE INTO meta VALUES('schema', ?)",
                  ("%s:%s" % (SCHEMA_VERSION, INDEXER_KEY),))
        c.commit()

    def _meta(self, key):
        r = self.db.execute("SELECT value FROM meta WHERE key=?", (key,)).fetchone()
        return r[0] if r else None

    # -- refresh --------------------------------------------------------------
    def refresh(self, rebuild=False):
        c = self.db
        if rebuild:
            for t in TABLES:
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
        if rebuild:
            c.execute("VACUUM")
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
                syms = symbols_of(path, text)
                self.db.executemany(
                    "INSERT INTO symbols(file, name, kind, line) VALUES(?,?,?,?)",
                    [(fid,) + s for s in syms])
                self.db.executemany(
                    "INSERT INTO occ(file, name, line, col, role) VALUES(?,?,?,?,?)",
                    [(fid,) + o for o in
                     occurrences_of(path, text, {(n, ln) for n, _, ln in syms})])
                self.db.executemany("INSERT INTO deps(file, spec, kind) VALUES(?,?,?)",
                                    [(fid,) + d for d in repo_deps.deps_of(path, text)])
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
        self.db.execute("DELETE FROM occ WHERE file=?", (fid,))
        self.db.execute("DELETE FROM deps WHERE file=?", (fid,))

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

    def where(self, name, limit=None):
        """Every occurrence of the identifier `name` (a qualified name looks up its last
        part): {"name", "exact", "keyword_in", "hits": [{path, line, col, role, text,
        dirty}]}, defs first, then imports, refs, text. Exact case wins; with none,
        case-insensitive (exact False). `keyword_in`: the languages where the name is a
        reserved word, whose files store no occurrence of it. Line text is read for the
        first `limit` hits only (None = all). Lines are HEAD's; `dirty` = the file has
        uncommitted edits."""
        bare = last_part(name)
        rows = [dict(r) for r in self.db.execute(
            "SELECT o.name, o.line, o.col, o.role, f.path, f.blob FROM occ o"
            " JOIN files f ON f.id = o.file WHERE o.name = ? COLLATE NOCASE", (bare,))]
        exact = [r for r in rows if r["name"] == bare]
        rows = exact or rows
        rows.sort(key=lambda r: (ROLES.index(r["role"]), r["path"], r["line"]))
        shown = rows if limit is None else rows[:limit]
        texts, keep = self._cat(r["blob"] for r in shown), {id(r) for r in shown}
        dirty = self._dirty() if rows else set()
        for r in rows:
            blob = r.pop("blob")
            lines = texts.get(blob, "").split("\n") if id(r) in keep else []
            r["text"] = lines[r["line"] - 1].strip()[:200] if r["line"] <= len(lines) else ""
            r["dirty"] = r["path"] in dirty
        return {"name": bare, "exact": bool(exact) or not rows, "hits": rows,
                "keyword_in": sorted(k for k, words in KEYWORDS.items() if bare in words)}

    def affected(self, changed):
        """The tests the changed paths reach (see repo_deps.affected). A path may be
        repo-relative or absolute."""
        root = os.path.realpath(self.repo)
        rel = []
        for c in changed:
            if os.path.isabs(c):
                c = os.path.relpath(os.path.realpath(c), root)
            c = posixpath.normpath(c.replace(os.sep, "/"))
            if not c.startswith(".."):
                rel.append(c)
        rows = self.db.execute("SELECT id, path, blob, kind FROM files").fetchall()
        files = {r["path"]: (r["id"], r["blob"]) for r in rows}  # vendored: a target, not parsed
        mf = {r["path"]: r["blob"] for r in rows if r["kind"] != "vendored" and
              posixpath.basename(r["path"]) in ("package.json", "Cargo.toml", "Move.toml")}
        texts = self._cat(mf.values())
        manifests = {p: texts.get(b, "") for p, b in mf.items()}
        by_id = {fid: p for p, (fid, _) in files.items()}
        deps = {}
        for r in self.db.execute("SELECT file, spec, kind FROM deps"):
            if r["file"] in by_id:
                deps.setdefault(by_id[r["file"]], []).append((r["spec"], r["kind"]))
        tree = repo_deps.Tree(files, manifests)
        return repo_deps.affected(tree, deps, manifests, list(dict.fromkeys(rel)))

    def changed_files(self, base=None):
        """Uncommitted edits (staged or not) and untracked files; with `base`, also every
        file changed on HEAD since its merge-base with `base`."""
        out = (git(self.repo, "diff", "--name-only", "HEAD") or b"").decode()
        out += (git(self.repo, "ls-files", "--others", "--exclude-standard") or b"").decode()
        if base:
            out += (git(self.repo, "diff", "--name-only", "%s...HEAD" % base) or b"").decode()
        return list(dict.fromkeys(p for p in out.split("\n") if p))

    def _dirty(self):
        out = git(self.repo, "status", "--porcelain=v1", "-z", "--untracked-files=no") or b""
        paths, recs = set(), out.decode("utf-8", "replace").split("\0")
        i = 0
        while i < len(recs):
            rec = recs[i]
            if len(rec) > 3:
                paths.add(rec[3:])
                if rec[0] in "RC":
                    i += 1
            i += 1
        return paths

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
