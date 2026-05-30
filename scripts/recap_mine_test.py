#!/usr/bin/env python3
"""
Deterministic window test for recap_mine.py (stdlib-only, dependency-free).

NOT wired into C++ ctest. Builds a throwaway git repo in a tempdir with commits
straddling the window boundary, runs the real recap_mine.py CLI as a subprocess,
and asserts it windows correctly + degrades gracefully without a gh remote.

Run manually: python3 scripts/recap_mine_test.py
Exit 0 = all pass; 1 = any failure; 0 + SKIP if git is unavailable.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
MINER = HERE / "recap_mine.py"

_failures = 0


def check(label, cond):
    global _failures
    if cond:
        print(f"PASS: {label}")
    else:
        print(f"FAIL: {label}")
        _failures += 1


def git(args, cwd, env=None):
    full = dict(os.environ)
    if env:
        full.update(env)
    subprocess.run(["git"] + args, cwd=cwd, env=full,
                   check=True, capture_output=True, text=True)


def commit(cwd, when, path, message):
    f = Path(cwd) / path
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_text(message + "\n")
    git(["add", path], cwd)
    env = {"GIT_AUTHOR_DATE": when, "GIT_COMMITTER_DATE": when}
    git(["commit", "-m", message], cwd, env)


def main():
    if shutil.which("git") is None:
        print("SKIP (no git)")
        return 0

    tmp = tempfile.mkdtemp(prefix="recap_mine_test_")
    try:
        # init a single-repo workspace on branch `main`
        git(["init"], tmp)
        git(["config", "user.email", "test@example.com"], tmp)
        git(["config", "user.name", "Recap Test"], tmp)
        git(["config", "init.defaultBranch", "main"], tmp)
        # rename whatever the default initial branch is to `main`
        git(["branch", "-M", "main"], tmp)

        commit(tmp, "2026-01-01T12:00:00", "old.txt", "out of window")     # excluded
        commit(tmp, "2026-05-20T12:00:00", "a.txt", "in window first")     # included
        commit(tmp, "2026-05-25T12:00:00", "src/b.txt", "in window second")  # included

        out = Path(tmp) / "out.json"
        subprocess.run(
            [sys.executable, str(MINER), "--root", tmp,
             "--since", "2026-05-01", "--out", str(out), "--quiet"],
            check=True, capture_output=True, text=True,
        )

        data = json.loads(out.read_text())

        check("schema == recap/tier1-timeline/v1",
              data.get("schema") == "recap/tier1-timeline/v1")
        check("exactly one repo (single-repo fallback)",
              len(data.get("repos", [])) == 1)

        repo = (data.get("repos") or [{}])[0]
        wc = repo.get("windowed_commits", [])

        check("exactly 2 windowed_commits (Jan excluded)", len(wc) == 2)
        subjects = {c["subject"] for c in wc}
        check("Jan out-of-window commit absent", "out of window" not in subjects)
        check("in-window subjects present",
              {"in window first", "in window second"} <= subjects)

        files_all = {f for c in wc for f in c.get("files", [])}
        check("all in-window commits carry non-empty files",
              all(c.get("files") for c in wc))
        check("a.txt present in files", "a.txt" in files_all)
        check("src/b.txt present in files", "src/b.txt" in files_all)

        check("window_start_date == 2026-05-20",
              data.get("window_start_date") == "2026-05-20")
        check("current_branch == main", repo.get("current_branch") == "main")
        check("last_commit.date == 2026-05-25",
              (repo.get("last_commit") or {}).get("date") == "2026-05-25")
        check("dirty_count == 0", repo.get("dirty_count") == 0)
        check("merged_prs_in_window == [] (graceful degradation)",
              repo.get("merged_prs_in_window") == [])
        check("open_prs == [] (graceful degradation)",
              repo.get("open_prs") == [])

        if _failures == 0:
            print("PASS: recap_mine window test")
            return 0
        print(f"FAIL: recap_mine window test ({_failures} failing assertion(s))")
        return 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
