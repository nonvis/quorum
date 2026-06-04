#!/usr/bin/env python3
"""
Historian Tier-1 miner (deterministic, read-only, no LLM).

Per top-level git repo in a workspace, mines the raw decision-history sources:
recent commits (git log), OPEN pull requests, and PRs MERGED to the default
branch (gh pr list) — the highest-signal "a change the team deliberately
accepted" record. Also notes a project-root Decision Log if present. Emits a
structured JSON record the LLM historian (Tier 2) interprets into a decisions
table (recognizing significant decisions vs. routine churn, pivots,
supersession chains).

Read-only: only `git log` / `git remote` / `gh pr list` / `gh repo view` (all
read-only) + file reads. Writes ONLY its output file. Never mutates a repo.
Requires `gh` authenticated (read access) for PR data; degrades gracefully
(empty PR lists) when `gh` or a git remote is absent.

Usage: historian_mine.py [--root <dir>] [--out <file>] [--commits N] [--quiet]
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


def mine_repo(repo: Path, n_commits: int) -> dict:
    orepo = owner_repo(repo)
    cur_branch = sh(["git", "-C", str(repo), "branch", "--show-current"])

    default_branch = "main"
    if orepo:
        db = sh(["gh", "repo", "view", orepo, "--json", "defaultBranchRef",
                 "--jq", ".defaultBranchRef.name"])
        if db:
            default_branch = db

    # recent commits (non-merge), hash|date|author|subject
    raw = sh(["git", "-C", str(repo), "log", "--no-merges",
              f"-n{n_commits}", "--date=short",
              "--pretty=format:%h\t%ad\t%an\t%s"])
    commits = []
    for line in raw.splitlines():
        parts = line.split("\t", 3)
        if len(parts) == 4:
            commits.append({"hash": parts[0], "date": parts[1],
                            "author": parts[2], "subject": parts[3]})

    open_prs, merged_prs = [], []
    if orepo:
        open_prs = gh_json(["pr", "list", "-R", orepo, "--state", "open",
                            "--limit", "50", "--json",
                            "number,title,author,createdAt,headRefName,url"])
        merged_prs = gh_json(["pr", "list", "-R", orepo, "--state", "merged",
                              "--base", default_branch, "--limit", "100", "--json",
                              "number,title,author,mergedAt,url"])

    # normalize author objects → login
    def norm(prs):
        for p in prs:
            a = p.get("author") or {}
            p["author"] = a.get("login") or a.get("name") or ""
        return prs

    return {
        "name": repo.name,
        "owner_repo": orepo,
        "default_branch": default_branch,
        "current_branch": cur_branch,
        "recent_commits": commits,
        "open_prs": norm(open_prs),
        "merged_to_default_prs": norm(merged_prs),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.getcwd())
    ap.add_argument("--out", default=None)
    ap.add_argument("--commits", type=int, default=30)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    root = Path(args.root).resolve()
    out = Path(args.out) if args.out else root / ".quorum" / "historian" / "decisions-raw.json"
    out.parent.mkdir(parents=True, exist_ok=True)

    # root-authoritative: if the project root is itself a repo, it IS the
    # workspace repo — vendored sub-repos (e.g. ceph/) must not shadow it.
    # Only a bare parent dir (no root .git) treats its subdirs as the repos
    # (the multi-repo workspace layout, e.g. bastion).
    if (root / ".git").exists():
        repos = [root]
    else:
        repos = [p for p in sorted(root.iterdir())
                 if p.is_dir() and p.name not in SKIP_TOP and (p / ".git").exists()]

    mined = [mine_repo(r, args.commits) for r in repos]
    decision_log = next((f for f in ["00 - Decision Log.md", "DECISIONS.md", "Decision Log.md"]
                         if (root / f).is_file()), None)

    record = {
        "schema": "historian/tier1-decisions/v1",
        "root": str(root),
        "mined_at_utc": sh(["date", "-u", "+%Y-%m-%dT%H:%M:%SZ"]),
        "gh_authed": sh(["gh", "auth", "token"]) != "",
        "decision_log": decision_log,
        "repo_count": len(mined),
        "repos": mined,
    }
    out.write_text(json.dumps(record, indent=2) + "\n")

    if not args.quiet:
        print(f"Historian Tier-1 record → {out}")
        print(f"root: {root}  ({len(mined)} repos)  decision_log: {decision_log or '(none)'}\n")
        for r in mined:
            print(f"  {r['name']}  ({r['owner_repo'] or 'no-remote'})  default={r['default_branch']}  "
                  f"on={r['current_branch']}")
            print(f"      commits(recent): {len(r['recent_commits'])}  "
                  f"open PRs: {len(r['open_prs'])}  merged→{r['default_branch']}: {len(r['merged_to_default_prs'])}")
            for p in r["merged_to_default_prs"][:3]:
                print(f"        merged #{p['number']} {p.get('mergedAt','')[:10]} {p['author']}: {p['title'][:60]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
