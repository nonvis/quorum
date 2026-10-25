#!/usr/bin/env python3
"""`quorum curate` — which memory files no session touched lately, and what
their index rows cost. Deterministic, $0, read-only.

The mechanical half of Hermes Agent's Curator (usage-tracked, stale after N
days): a file is touched when a session or one of its subagents Read, Edited or
Wrote it inside the window. Untouched is not stale: a memory can do its whole job
through its one-line row in MEMORY*.md, which every session loads without a
Read. So the report pairs each file with the bytes of the index rows that name it
(MEMORY.md's are paid by every session, MEMORY_<X>.md's when that index is read), and the owner decides keep / compress /
archive. Nothing is moved or rewritten here.

The window is bounded by the transcripts on this host: Claude Code keeps about
30 days, so the report states the oldest session it saw.
"""
import json
import os
import re
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROW_LINK = re.compile(r"\]\(([^)\s]+\.md)\)|\[\[([^\]|#]+)\]\]")
TOOLS = {"Read": "reads", "Edit": "edits", "Write": "edits"}


def _iso(dt):
    return dt.strftime("%Y-%m-%dT%H:%M:%SZ")


def _index_rows(memory):
    """-> [(index file name, row bytes, {file names the row links})]."""
    out = []
    for idx in sorted(memory.glob("MEMORY*.md")):
        for line in idx.read_text(encoding="utf-8", errors="replace").splitlines(True):
            names = {Path(md).name if md else slug.strip() + ".md"
                     for md, slug in ROW_LINK.findall(line)}
            if names:
                out.append((idx.name, len(line.encode("utf-8")), names))
    return out


def _uses(tdirs, memory, since):
    """-> ({memory file name: {"reads", "edits", "last"}}, sessions, oldest ts)."""
    mem = os.path.realpath(str(memory)) + "/"
    uses, sessions, oldest = {}, 0, None
    for tdir in tdirs:
        for main in sorted(Path(tdir).glob("*.jsonl")):
            files = [main] + sorted((main.parent / main.stem / "subagents").glob("*.jsonl"))
            seen_in_window = False
            for f in files:
                try:
                    lines = f.read_text(errors="replace").splitlines()
                except OSError:
                    continue
                for line in lines:
                    try:
                        r = json.loads(line)
                    except ValueError:
                        continue
                    ts = r.get("timestamp") or ""
                    if ts and (oldest is None or ts < oldest):
                        oldest = ts
                    if not ts or ts < since:
                        continue
                    seen_in_window = True
                    content = (r.get("message") or {}).get("content")
                    if not isinstance(content, list):
                        continue
                    for c in content:
                        if not (isinstance(c, dict) and c.get("type") == "tool_use"
                                and c.get("name") in TOOLS):
                            continue
                        p = os.path.realpath((c.get("input") or {}).get("file_path") or "")
                        if not p.startswith(mem):
                            continue
                        u = uses.setdefault(p[len(mem):], {"reads": 0, "edits": 0, "last": ""})
                        u[TOOLS[c["name"]]] += 1
                        u["last"] = max(u["last"], ts)
            sessions += seen_in_window
    return uses, sessions, oldest


def curate(memory, tdirs, now=None, days=30):
    memory = Path(memory)
    now_dt = (datetime.strptime(now, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
              if now else datetime.now(timezone.utc))
    since = _iso(now_dt - timedelta(days=days))
    uses, sessions, oldest = _uses(tdirs, memory, since)
    rows = _index_rows(memory)
    files = []
    for f in sorted(memory.glob("*.md")):
        if f.name.startswith("MEMORY"):
            continue
        u = uses.get(f.name, {"reads": 0, "edits": 0, "last": ""})
        mine = [(ix, b) for ix, b, names in rows if f.name in names]
        files.append({"file": f.name, "reads": u["reads"], "edits": u["edits"],
                      "last": u["last"], "indexes": sorted({ix for ix, _ in mine}),
                      "row_bytes": sum(b for _, b in mine), "bytes": f.stat().st_size})
    files.sort(key=lambda r: (r["reads"] + r["edits"] > 0, -r["row_bytes"], r["file"]))
    # A row whose every file went untouched: what it costs, counted once.
    # MEMORY.md is loaded by every session; MEMORY_<X>.md only when read.
    touched = {f["file"] for f in files if f["reads"] + f["edits"]}
    idle_rows = {"auto": 0, "second_level": 0}
    for ix, b, names in rows:
        if not names & touched:
            idle_rows["auto" if ix == "MEMORY.md" else "second_level"] += b
    return {"window": {"from": since, "to": _iso(now_dt), "days": days,
                       "sessions": sessions, "oldest_transcript": oldest},
            "files": files, "idle_rows": idle_rows}
