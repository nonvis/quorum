"""Corpus discovery, chunking, and the SQLite FTS5 index.

The corpus is a Quorum project's accumulated knowledge, per the scope
hierarchy the daemon's context assembler uses:

    .quorum/vaults/<agent>/knowledge/*.md    (agent scope — the knower vaults)
    .quorum/knowledge/roles/<role>/*.md      (role scope)
    .quorum/knowledge/*.md                   (project scope)

Notes are chunked by markdown heading so retrieval returns sections, not
whole files. File-level frontmatter (tags + summary) is attached to every
chunk and weighted ABOVE body text at query time — mirroring the daemon's
"filename x3 + content x1" scoring philosophy.

The index lives inside the project (.quorum/own-agent/index.db) and reindexing
is incremental by mtime, so `ask`/`search` can afford to refresh it on every
invocation.
"""

from __future__ import annotations

import re
import sqlite3
from pathlib import Path

INDEX_REL = Path(".quorum") / "own-agent" / "index.db"

# bm25 column weights: heading, tags, summary, fname, content
BM25_WEIGHTS = "3.0, 3.0, 2.0, 3.0, 1.0"


def index_path(project: Path) -> Path:
    return project / INDEX_REL


def corpus_files(project: Path) -> list[tuple[str, Path]]:
    """Every knowledge note as (owner, absolute path), scope order preserved."""
    q = project / ".quorum"
    out: list[tuple[str, Path]] = []

    vaults = q / "vaults"
    if vaults.is_dir():
        for vault in sorted(p for p in vaults.iterdir() if p.is_dir()):
            kdir = vault / "knowledge"
            if kdir.is_dir():
                out.extend((vault.name, f) for f in sorted(kdir.glob("*.md")))

    roles = q / "knowledge" / "roles"
    if roles.is_dir():
        for role in sorted(p for p in roles.iterdir() if p.is_dir()):
            out.extend((f"role:{role.name}", f) for f in sorted(role.glob("*.md")))

    proj = q / "knowledge"
    if proj.is_dir():
        out.extend(("project", f) for f in sorted(proj.glob("*.md")))

    return out


_FRONTMATTER_RE = re.compile(r"\A---\n(.*?)\n---\n?", re.S)


def parse_frontmatter(text: str) -> tuple[dict[str, str], str]:
    """Flat YAML frontmatter -> dict, plus the remaining body.

    Handles the two shapes knowers actually write: `key: value` and block
    scalars (`summary: >` followed by indented lines). Not a YAML parser on
    purpose — same trade the C++ side makes.
    """
    m = _FRONTMATTER_RE.match(text)
    if not m:
        return {}, text
    body = text[m.end() :]
    fm: dict[str, str] = {}
    block_key: str | None = None
    block_lines: list[str] = []
    for line in m.group(1).split("\n"):
        if block_key is not None:
            if line.startswith((" ", "\t")):
                block_lines.append(line.strip())
                continue
            fm[block_key] = " ".join(block_lines)
            block_key = None
        km = re.match(r"^([A-Za-z_][\w-]*):\s*(.*)$", line)
        if not km:
            continue
        key, val = km.group(1), km.group(2).strip()
        if val in (">", "|", ">-", "|-"):
            block_key, block_lines = key, []
        else:
            fm[key] = val
    if block_key is not None:
        fm[block_key] = " ".join(block_lines)
    return fm, body


def chunk_body(body: str) -> list[tuple[str, str]]:
    """Split a note body into (heading, text) chunks at #/##/### headings."""
    chunks: list[tuple[str, str]] = []
    heading = "(intro)"
    buf: list[str] = []
    for line in body.split("\n"):
        hm = re.match(r"^(#{1,3})\s+(.+)$", line)
        if hm:
            if "".join(buf).strip():
                chunks.append((heading, "\n".join(buf).strip()))
            heading = hm.group(2).strip()
            buf = [line]
        else:
            buf.append(line)
    if "".join(buf).strip():
        chunks.append((heading, "\n".join(buf).strip()))
    return chunks


def _open(project: Path) -> sqlite3.Connection:
    db_file = index_path(project)
    db_file.parent.mkdir(parents=True, exist_ok=True)
    con = sqlite3.connect(db_file)
    con.execute(
        "CREATE TABLE IF NOT EXISTS files("
        "  path TEXT PRIMARY KEY, mtime REAL NOT NULL, owner TEXT NOT NULL)"
    )
    con.execute(
        "CREATE VIRTUAL TABLE IF NOT EXISTS chunks USING fts5("
        "  heading, tags, summary, fname, content,"
        "  path UNINDEXED, owner UNINDEXED,"
        "  tokenize='porter unicode61')"
    )
    return con


def index_project(project: Path) -> dict[str, int]:
    """Incremental reindex. Returns counts for the CLI to report."""
    project = project.resolve()
    con = _open(project)
    stats = {"indexed": 0, "unchanged": 0, "removed": 0, "chunks": 0}

    seen: set[str] = set()
    known = dict(con.execute("SELECT path, mtime FROM files"))

    for owner, f in corpus_files(project):
        rel = str(f.relative_to(project))
        seen.add(rel)
        mtime = f.stat().st_mtime
        if known.get(rel) == mtime:
            stats["unchanged"] += 1
            continue
        fm, body = parse_frontmatter(f.read_text(encoding="utf-8", errors="replace"))
        tags = fm.get("tags", "").strip("[]")
        summary = fm.get("summary", "")
        con.execute("DELETE FROM chunks WHERE path = ?", (rel,))
        for heading, text in chunk_body(body):
            con.execute(
                "INSERT INTO chunks(heading, tags, summary, fname, content, path, owner)"
                " VALUES(?,?,?,?,?,?,?)",
                (heading, tags, summary, f.stem, text, rel, owner),
            )
            stats["chunks"] += 1
        con.execute(
            "INSERT INTO files(path, mtime, owner) VALUES(?,?,?)"
            " ON CONFLICT(path) DO UPDATE SET mtime=excluded.mtime, owner=excluded.owner",
            (rel, mtime, owner),
        )
        stats["indexed"] += 1

    for rel in set(known) - seen:
        con.execute("DELETE FROM chunks WHERE path = ?", (rel,))
        con.execute("DELETE FROM files WHERE path = ?", (rel,))
        stats["removed"] += 1

    con.commit()
    con.close()
    return stats
