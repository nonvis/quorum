#!/usr/bin/env python3
"""
Recap Tier-1 miner (deterministic, read-only, no LLM) — WINDOWED.

Per top-level git repo in a workspace, mines the raw WHAT/WHEN catch-up sources
over a window (--since): windowed commits with their changed files (git log
--name-only), PRs merged within the window + open PRs (gh), and the
where-I-left-off mechanical facts git can show (current branch, last commit,
dirty/ahead/stash counts). Emits a DATED windowed timeline the LLM recap (Tier
2) interprets into one component-grouped timeline + a where-I-left-off draft.

Seam vs historian: historian = WHY / all-time decision record
(.quorum/historian/decisions-raw.json). recap = WHAT-WHEN / windowed dated
timeline (.quorum/recap/timeline-raw.json) — distinct file, distinct schema.

The git timeline is ALWAYS emitted — gh ONLY enriches it with merged/open PR
data. With no gh / no auth / no remote, the windowed_commits + where-I-left-off
facts are still produced and the PR lists degrade gracefully to empty. git is
the spine; gh is enrichment.

window_start_date (used to filter merged PRs, which gh returns unbounded) is
derived dependency-free: it is the MIN date across all in-window commits across
ALL repos, falling back to today when the window has zero commits (an empty PR
window is then correct — nothing happened). No date library; self-consistent
with the window git itself returned.

Read-only: only `git log` / `git remote` / `git branch` / `git status` /
`git rev-list` / `git stash list` / `gh pr list` / `gh repo view` + file reads.
Writes ONLY its output file. Never mutates a repo.

Usage: recap_mine.py [--root <dir>] [--out <file>] [--since "<range>"] [--quiet]
"""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

SKIP_TOP = {".quorum", ".idea", ".git", ".vscode", "node_modules"}


def sh(args, cwd=None, timeout=30):
    try:
        r = subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=timeout)
        return r.stdout.strip() if r.returncode == 0 else ""
    except Exception:
        return ""


def owner_repo(repo: Path):
    url = sh(["git", "-C", str(repo), "remote", "get-url", "origin"])
    if not url:
        return None
    # https://github.com/owner/repo(.git)  OR  git@github.com:owner/repo(.git)
    m = re.search(r"github\.com[:/]([^/]+/[^/]+?)(?:\.git)?$", url)
    return m.group(1) if m else None


def gh_json(args, timeout=40):
    out = sh(["gh"] + args, timeout=timeout)
    if not out:
        return []
    try:
        return json.loads(out)
    except Exception:
        return []


def norm(prs):
    # normalize author objects → login
    for p in prs:
        a = p.get("author") or {}
        p["author"] = a.get("login") or a.get("name") or ""
    return prs


def mine_commits(repo: Path, since: str) -> dict:
    """Pass 1: windowed commits with changed files + where-I-left-off facts.

    PR data is deferred to pass 2 (needs the global window_start_date).
    """
    orepo = owner_repo(repo)
    cur_branch = sh(["git", "-C", str(repo), "branch", "--show-current"])

    default_branch = "main"
    if orepo:
        db = sh(["gh", "repo", "view", orepo, "--json", "defaultBranchRef",
                 "--jq", ".defaultBranchRef.name"])
        if db:
            default_branch = db

    # windowed commits with changed files: a record-separator (RS, \x1e) starts
    # each commit; unit-separators (US, \x1f) delimit fields; the lines that
    # follow (from --name-only) until the next RS are the changed file paths.
    raw = sh(["git", "-C", str(repo), "log", "--no-merges",
              f"--since={since}", "--date=short",
              "--pretty=format:%x1ecommit%x1f%h%x1f%ad%x1f%an%x1f%s",
              "--name-only"])
    windowed_commits = []
    for record in raw.split("\x1e"):
        if not record.strip():
            continue
        lines = record.split("\n")
        header = lines[0].split("\x1f")
        if len(header) != 5 or header[0] != "commit":
            continue
        files = [ln for ln in lines[1:] if ln.strip()]
        windowed_commits.append({
            "hash": header[1],
            "date": header[2],
            "author": header[3],
            "subject": header[4],
            "files": files,
        })

    # where-I-left-off mechanical facts
    last_raw = sh(["git", "-C", str(repo), "log", "-1", "--date=short",
                   "--pretty=format:%h\t%ad\t%s"])
    lp = last_raw.split("\t", 2)
    last_commit = ({"hash": lp[0], "date": lp[1], "subject": lp[2]}
                   if len(lp) == 3 else {"hash": "", "date": "", "subject": ""})

    status = sh(["git", "-C", str(repo), "status", "--porcelain"])
    dirty_count = len([ln for ln in status.splitlines() if ln.strip()]) if status else 0

    ahead_count = int(sh(["git", "-C", str(repo), "rev-list", "--count",
                          "@{u}..HEAD"]) or 0)

    stash = sh(["git", "-C", str(repo), "stash", "list"])
    stash_count = len([ln for ln in stash.splitlines() if ln.strip()]) if stash else 0

    return {
        "name": repo.name,
        "owner_repo": orepo,
        "default_branch": default_branch,
        "current_branch": cur_branch,
        "last_commit": last_commit,
        "dirty_count": dirty_count,
        "ahead_count": ahead_count,
        "stash_count": stash_count,
        "windowed_commits": windowed_commits,
    }


def mine_prs(rec: dict, window_start_date: str) -> dict:
    """Pass 2: merged-in-window + open PRs, filtered by the global window."""
    orepo = rec["owner_repo"]
    default_branch = rec["default_branch"]
    merged_prs_in_window, open_prs = [], []
    if orepo:
        open_prs = norm(gh_json(["pr", "list", "-R", orepo, "--state", "open",
                                 "--limit", "50", "--json",
                                 "number,title,author,createdAt,headRefName,url"]))
        merged = norm(gh_json(["pr", "list", "-R", orepo, "--state", "merged",
                               "--base", default_branch, "--limit", "100", "--json",
                               "number,title,author,mergedAt,url"]))
        merged_prs_in_window = [p for p in merged
                                if (p.get("mergedAt") or "")[:10] >= window_start_date]
    rec["merged_prs_in_window"] = merged_prs_in_window
    rec["open_prs"] = open_prs
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.getcwd())
    ap.add_argument("--out", default=None)
    ap.add_argument("--since", default="1 month ago")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    root = Path(args.root).resolve()
    out = Path(args.out) if args.out else root / ".quorum" / "recap" / "timeline-raw.json"
    out.parent.mkdir(parents=True, exist_ok=True)

    repos = [p for p in sorted(root.iterdir())
             if p.is_dir() and p.name not in SKIP_TOP and (p / ".git").exists()]
    # single-repo workspace: the root itself is the repo
    if not repos and (root / ".git").exists():
        repos = [root]

    # pass 1: windowed commits + where-I-left-off facts per repo
    mined = [mine_commits(r, args.since) for r in repos]

    # window anchor: MIN date across all in-window commits across ALL repos,
    # else today (no commits → no PRs to surface anyway).
    all_dates = [c["date"] for r in mined for c in r["windowed_commits"] if c["date"]]
    window_start_date = min(all_dates) if all_dates else sh(["date", "+%Y-%m-%d"])

    # pass 2: PRs filtered to the derived window
    mined = [mine_prs(r, window_start_date) for r in mined]

    record = {
        "schema": "recap/tier1-timeline/v1",
        "root": str(root),
        "mined_at_utc": sh(["date", "-u", "+%Y-%m-%dT%H:%M:%SZ"]),
        "since": args.since,
        "window_start_date": window_start_date,
        "gh_authed": sh(["gh", "auth", "token"]) != "",
        "repo_count": len(mined),
        "repos": mined,
    }
    out.write_text(json.dumps(record, indent=2) + "\n")

    if not args.quiet:
        print(f"Recap Tier-1 timeline → {out}")
        print(f"root: {root}  ({len(mined)} repos)  since: {args.since!r}  "
              f"window_start: {window_start_date}\n")
        for r in mined:
            lc = r["last_commit"]
            print(f"  {r['name']}  ({r['owner_repo'] or 'no-remote'})  "
                  f"default={r['default_branch']}  on={r['current_branch']}")
            print(f"      windowed commits: {len(r['windowed_commits'])}  "
                  f"merged PRs(window): {len(r['merged_prs_in_window'])}  "
                  f"open PRs: {len(r['open_prs'])}")
            print(f"      last: {lc['hash']} {lc['date']} {lc['subject'][:60]}")
            extras = []
            if r["dirty_count"]:
                extras.append(f"dirty: {r['dirty_count']}")
            if r["ahead_count"]:
                extras.append(f"ahead: {r['ahead_count']}")
            if r["stash_count"]:
                extras.append(f"stash: {r['stash_count']}")
            if extras:
                print(f"      {'  '.join(extras)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
