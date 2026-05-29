#!/usr/bin/env python3
"""
Cartographer Tier-1 indexer (deterministic, read-only, no LLM).

Scans a workspace's top-level folders and, for each, captures: detected
languages (from manifests AND source-file extensions, found via a bounded
recursive walk), git branch + tracked-file count, README headline, the
immediate subfolders, the key files/folders, and any CLAUDE.md files. Emits a
structured JSON layout index. Cheap enough to run many times a day; the LLM
cartographer (Tier 2) reads this + the CLAUDE.md(s) and annotates
purpose/importance while honoring CLAUDE.md rules.

Read-only: reads the filesystem + read-only git only. Writes ONLY the output
file. Never mutates any repo.

Usage:
  cartographer_index.py [--root <dir>] [--out <file>] [--max-depth N] [--quiet]
"""
import argparse
import json
import os
import subprocess
import sys
from collections import Counter
from pathlib import Path

SKIP_TOP = {".quorum", ".idea", ".git", ".vscode", "node_modules", ".DS_Store"}
PRUNE = {"node_modules", "target", "build", "dist", ".git", ".next", "out",
         "vendor", ".turbo", "coverage", "__pycache__", ".venv", "venv",
         ".idea", ".vscode", "artifacts", "cache"}

MANIFEST_LANG = {
    "package.json": "TypeScript/Node", "tsconfig.json": "TypeScript",
    "Cargo.toml": "Rust", "Anchor.toml": "Rust/Anchor (Solana)",
    "Move.toml": "Move (Sui)", "foundry.toml": "Solidity/Foundry",
    "hardhat.config.js": "Solidity/Hardhat", "hardhat.config.ts": "Solidity/Hardhat",
    "go.mod": "Go", "pyproject.toml": "Python", "requirements.txt": "Python",
}
# source extension -> (language, min count to report)
EXT_LANG = {
    ".sol": ("Solidity", 1), ".move": ("Move (Sui)", 1), ".rs": ("Rust", 1),
    ".go": ("Go", 1), ".ts": ("TypeScript", 3), ".tsx": ("TypeScript", 3),
    ".js": ("JavaScript", 5), ".py": ("Python", 3),
}
KEY_SUBDIRS = {"src", "sources", "programs", "contracts", "lib", "app",
               "scripts", "test", "tests", "config", "migrations", "cmd"}


def sh(args, cwd=None):
    try:
        out = subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=15)
        return out.stdout.strip() if out.returncode == 0 else ""
    except Exception:
        return ""


def readme_headline(folder: Path) -> str:
    for name in ("README.md", "README.MD", "Readme.md", "README", "README.txt"):
        p = folder / name
        if p.is_file():
            try:
                for line in p.read_text(errors="replace").splitlines():
                    s = line.strip().lstrip("#").strip()
                    if s:
                        return s[:120]
            except Exception:
                pass
            return ""
    return ""


def walk(folder: Path, max_depth: int):
    """Bounded recursive walk: collect manifests, ext counts, CLAUDE.md paths."""
    manifests, claude_mds = [], []
    ext_counts = Counter()
    base_parts = len(folder.parts)
    for dirpath, dirnames, filenames in os.walk(folder):
        depth = len(Path(dirpath).parts) - base_parts
        if depth >= max_depth:
            dirnames[:] = []
        dirnames[:] = [d for d in dirnames if d not in PRUNE and not d.startswith(".")]
        for fn in filenames:
            rel = str(Path(dirpath, fn).relative_to(folder))
            if fn in MANIFEST_LANG:
                manifests.append(rel)
            elif fn == "CLAUDE.md":
                claude_mds.append(rel)
            ext = Path(fn).suffix.lower()
            if ext in EXT_LANG:
                ext_counts[ext] += 1
    return manifests, ext_counts, claude_mds


def index_component(folder: Path, max_depth: int) -> dict:
    manifests, ext_counts, claude_mds = walk(folder, max_depth)

    languages = []
    for m in manifests:
        lang = MANIFEST_LANG.get(Path(m).name)
        if lang and lang not in languages:
            languages.append(lang)
    for ext, (lang, minc) in EXT_LANG.items():
        if ext_counts.get(ext, 0) >= minc and lang not in languages:
            languages.append(lang)

    subdirs = sorted(p.name for p in folder.iterdir()
                     if p.is_dir() and not p.name.startswith("."))
    key_subdirs = [d for d in subdirs if d in KEY_SUBDIRS]

    is_repo = (folder / ".git").exists()
    branch = sh(["git", "-C", str(folder), "branch", "--show-current"]) if is_repo else ""
    tracked = sh(["git", "-C", str(folder), "ls-files"]) if is_repo else ""
    dirty = sh(["git", "-C", str(folder), "status", "--porcelain"]) if is_repo else ""

    return {
        "name": folder.name,
        "is_git_repo": is_repo,
        "branch": branch,
        "tracked_files": len(tracked.splitlines()) if tracked else 0,
        "dirty_files": len(dirty.splitlines()) if dirty else 0,
        "languages": languages or ["(undetected)"],
        "manifests": sorted(manifests),
        "ext_counts": dict(sorted(ext_counts.items())),
        "readme": readme_headline(folder),
        "subdirs": subdirs,
        "key_subdirs": key_subdirs,
        "claude_mds": sorted(claude_mds),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.getcwd())
    ap.add_argument("--out", default=None)
    ap.add_argument("--max-depth", type=int, default=4)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    root = Path(args.root).resolve()
    out = Path(args.out) if args.out else root / ".quorum" / "cartographer" / "layout.json"
    out.parent.mkdir(parents=True, exist_ok=True)

    components = [index_component(p, args.max_depth)
                  for p in sorted(root.iterdir())
                  if p.is_dir() and p.name not in SKIP_TOP]

    workspace_claude_md = "CLAUDE.md" if (root / "CLAUDE.md").is_file() else None

    index = {
        "schema": "cartographer/tier1-layout/v1",
        "root": str(root),
        "indexed_at_utc": sh(["date", "-u", "+%Y-%m-%dT%H:%M:%SZ"]),
        "workspace_claude_md": workspace_claude_md,
        "component_count": len(components),
        "components": components,
    }
    out.write_text(json.dumps(index, indent=2) + "\n")

    if not args.quiet:
        print(f"Cartographer Tier-1 index → {out}")
        wcm = workspace_claude_md or "(none)"
        print(f"root: {root}  ({len(components)} components)  workspace CLAUDE.md: {wcm}\n")
        for c in components:
            repo = f"[{c['branch']}]" if c["is_git_repo"] else "(not a repo)"
            print(f"  {c['name']}  — {', '.join(c['languages'])}  {repo}  {c['tracked_files']} files")
            if c["readme"]:
                print(f"      {c['readme']}")
            if c["key_subdirs"]:
                print(f"      key: {', '.join(c['key_subdirs'])}")
            if c["claude_mds"]:
                print(f"      CLAUDE.md: {', '.join(c['claude_mds'])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
