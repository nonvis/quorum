#!/usr/bin/env python3
"""Replay real prompts through the hook's matcher — `quorum hook --replay`.

The answer key is what the sessions did: for each prompt the operator typed in a
vault session, the vault and memory notes that session (and its subagents) then
Read before the next prompt. Each prompt goes through Index.prompt_hits on
today's index, with the session's earlier hits as `seen` (as the hook reads them
from hook.log), and a hit counts as right when that turn Read it. A note first
committed after the prompt can be neither a hit nor an answer (it did not exist
yet). Read-only, $0: transcripts, the index, the vault's git log.
"""
import json
import os
import subprocess
from pathlib import Path

import vault_index


def transcript_dir(vault):
    """Claude Code names the dir from the path as the session saw it, which may
    differ from the resolved one (macOS: /var -> /private/var)."""
    for v in (os.environ.get("QUORUM_VAULT") or "", str(vault)):
        if v:
            cand = Path(os.path.expanduser("~/.claude/projects")) / "".join(
                "-" if c in "/." else c for c in os.path.expanduser(v))
            if cand.is_dir():
                return cand
    return None


def _records(path):
    try:
        lines = path.read_text(errors="replace").splitlines()
    except OSError:
        return
    for line in lines:
        try:
            yield json.loads(line)
        except ValueError:
            pass


def _prompt_text(r):
    """The operator's prompt in a user record, or None (tool results, meta,
    compaction summaries, harness messages)."""
    if r.get("type") != "user" or r.get("isMeta") or r.get("isCompactSummary") \
            or r.get("toolUseResult") is not None:
        return None
    c = (r.get("message") or {}).get("content")
    if isinstance(c, list):
        if any(isinstance(x, dict) and x.get("type") == "tool_result" for x in c):
            return None
        c = "\n".join(x.get("text", "") for x in c
                      if isinstance(x, dict) and x.get("type") == "text")
    if not isinstance(c, str) or not c.strip():
        return None
    if c.lstrip().startswith(vault_index.HARNESS_PREFIXES):
        return None
    return c


def _reads(files):
    out = []
    for f in files:
        for r in _records(f):
            content = (r.get("message") or {}).get("content")
            if not isinstance(content, list):
                continue
            for c in content:
                if isinstance(c, dict) and c.get("type") == "tool_use" \
                        and c.get("name") == "Read" and (c.get("input") or {}).get("file_path"):
                    out.append((r.get("timestamp", ""),
                                os.path.realpath(c["input"]["file_path"])))
    return out


_ADDED = {}  # repo top -> {repo-relative path: first-add time}


def _first_adds(top):
    if top not in _ADDED:
        out = subprocess.run(
            ["git", "-c", "core.quotePath=false", "-C", top, "log", "--diff-filter=A",
             "--no-renames", "--name-only", "--format=@%ad",
             "--date=format-local:%Y-%m-%dT%H:%M:%S"],
            capture_output=True, text=True, env=dict(os.environ, TZ="UTC")).stdout
        added, when = {}, ""
        for line in out.splitlines():
            if line.startswith("@"):
                when = line[1:]
            elif line:
                added[line] = when  # newest first: the last one written is the first add
        _ADDED[top] = added
    return _ADDED[top]


def _born(path, top):
    """When a note first entered the vault's git history, as a UTC ISO string
    ('' when unknown: no repo, untracked, a memory file). Not the file's birth
    time: a rewrite (an archive move, a lint fix) gives an old note a new one."""
    if not top or not path.startswith(top + "/"):
        return ""
    return _first_adds(top).get(path[len(top) + 1:], "")


def replay(idx, tdir, sessions=40):
    """-> {"turns": [...], "summary": {...}} over the newest `sessions` main
    transcripts in tdir."""
    vault = os.path.realpath(str(idx.vault))
    memory = os.path.realpath(str(idx.memory)) if idx.memory else None

    top = subprocess.run(["git", "-C", vault, "rev-parse", "--show-toplevel"],
                         capture_output=True, text=True).stdout.strip()
    top = os.path.realpath(top) if top else ""

    def real(hit):
        src, rel = hit.split(":", 1)
        return os.path.realpath(os.path.join(vault if src == "vault" else memory or "", rel))

    def label(p):
        if p.startswith(vault + "/"):
            return "vault:" + p[len(vault) + 1:]
        return "memory:" + p[len(memory) + 1:]

    def is_note(p):
        return p.endswith(".md") and (p.startswith(vault + "/")
                                      or (memory and p.startswith(memory + "/")))

    mains = sorted(Path(tdir).glob("*.jsonl"), key=lambda p: p.stat().st_mtime)[-sessions:] \
        if tdir else []
    turns = []
    for main in mains:
        prompts = [(r.get("timestamp", ""), t) for r in _records(main)
                   for t in [_prompt_text(r)] if t is not None]
        if not prompts:
            continue
        reads = _reads([main] + sorted((main.parent / main.stem / "subagents").glob("*.jsonl")))
        seen = set()
        for i, (ts, text) in enumerate(prompts):
            end = prompts[i + 1][0] if i + 1 < len(prompts) else "~"
            stamp = ts[:19]
            needed = {label(p) for t, p in reads if ts <= t < end and is_note(p)
                      and not _born(p, top) > stamp}
            _, hits = idx.prompt_hits(text, seen=seen)
            shown = [h["path"] for h in hits if not _born(real(h["path"]), top) > stamp]
            seen.update(shown)
            turns.append({"session": main.stem, "ts": ts, "prompt": text,
                          "first": i == 0, "hits": shown,
                          "right": [h for h in shown if h in needed],
                          "needed": sorted(needed)})
    fired = [t for t in turns if t["hits"]]
    s = {"sessions": len({t["session"] for t in turns}), "prompts": len(turns),
         "needing": sum(1 for t in turns if t["needed"]),
         "fired": len(fired), "useful": sum(1 for t in fired if t["right"]),
         "hits": sum(len(t["hits"]) for t in fired),
         "hits_right": sum(len(t["right"]) for t in fired)}
    s["precision"] = round(s["hits_right"] / s["hits"], 3) if s["hits"] else None
    s["coverage"] = round(s["useful"] / s["needing"], 3) if s["needing"] else None
    return {"turns": turns, "summary": s}
