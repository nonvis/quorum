"""Retrieval over the FTS5 index — the agent's tools live here.

Three tools, deliberately few:
    search(query)  — BM25-ranked chunks (heading/tags/summary/fname weighted
                     above body content)
    read(path)     — one full note, path-validated inside .quorum/
    map()          — corpus overview (every note + owner + summary)
"""

from __future__ import annotations

import re
import sqlite3
from pathlib import Path

from indexer import BM25_WEIGHTS, index_path


def sanitize_query(q: str) -> str:
    """Free text -> a safe FTS5 OR-query (punctuation breaks MATCH syntax)."""
    tokens: list[str] = []
    for t in re.findall(r"[A-Za-z0-9_]{2,}", q.lower()):
        if t not in tokens:
            tokens.append(t)
    return " OR ".join(tokens[:24])


def _connect(project: Path) -> sqlite3.Connection:
    db = index_path(project)
    if not db.exists():
        raise FileNotFoundError(f"no index at {db} — run `index` first")
    con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    con.row_factory = sqlite3.Row
    return con


def search(project: Path, query: str, k: int = 6) -> list[dict]:
    fts = sanitize_query(query)
    if not fts:
        return []
    con = _connect(project)
    rows = con.execute(
        f"SELECT heading, tags, summary, content, path, owner,"
        f"       bm25(chunks, {BM25_WEIGHTS}) AS score"
        f" FROM chunks WHERE chunks MATCH ? ORDER BY score LIMIT ?",
        (fts, k),
    ).fetchall()
    con.close()
    return [dict(r) for r in rows]


def read_note(project: Path, rel: str, cap: int = 12000) -> str:
    """Full note text. Only .md files inside the project's .quorum/ are readable."""
    target = (project / rel).resolve()
    quorum_root = (project / ".quorum").resolve()
    if quorum_root not in target.parents:
        raise PermissionError(f"refusing to read outside .quorum/: {rel}")
    if target.suffix != ".md" or not target.is_file():
        raise FileNotFoundError(f"not a readable note: {rel}")
    text = target.read_text(encoding="utf-8", errors="replace")
    if len(text) > cap:
        text = text[:cap] + f"\n… [truncated at {cap} chars — {len(text)} total]"
    return text


def corpus_map(project: Path) -> list[dict]:
    con = _connect(project)
    rows = con.execute(
        "SELECT path, owner, summary FROM chunks GROUP BY path ORDER BY owner, path"
    ).fetchall()
    con.close()
    return [dict(r) for r in rows]


# ── observation formatting (what the model actually sees) ────────────────

def format_hits(hits: list[dict], per_chunk: int = 700) -> str:
    if not hits:
        return "(no matches)"
    parts = []
    for i, h in enumerate(hits, 1):
        body = h["content"]
        if len(body) > per_chunk:
            body = body[:per_chunk] + " …"
        parts.append(
            f"[{i}] {h['path']} (owner: {h['owner']}) § {h['heading']}\n{body}"
        )
    return "\n\n".join(parts)


def format_map(entries: list[dict]) -> str:
    if not entries:
        return "(empty corpus — no knowledge notes indexed)"
    parts = []
    for e in entries:
        summary = (e["summary"] or "(no summary)").strip()
        if len(summary) > 220:
            summary = summary[:220] + " …"
        parts.append(f"- {e['path']} (owner: {e['owner']}) — {summary}")
    return "\n".join(parts)
