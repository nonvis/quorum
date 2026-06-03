#!/usr/bin/env python3
"""
Advisor vault scanner (deterministic, read-only, no LLM, no network).

Walks an external second-brain markdown vault (Obsidian/PARA) and emits a
COMPACT digest: the folder tree as headings, and under each folder its notes as
`- <name>: <summary>` where <summary> is the note's `summary:` frontmatter value
(empty when absent). NEVER reads full note bodies — only the leading frontmatter
block for the `summary:` line. Cheap enough to run on every advisor setup.

This digest is the cheap metadata gather feeding the ONE token-spending LLM
scoping pass in setup-advisor.sh — the model picks the relevant folders/areas
from this tree to write the advisor's `## Vault Scope`.

Read-only: reads the filesystem only. Writes nothing. Skips .obsidian/ and .git/.

Usage:
  advisor_vault_scan.py <vault-root> [--max-files-per-dir N]
"""
import argparse
import os
import sys
from pathlib import Path

SKIP_DIRS = {".obsidian", ".git", ".trash", ".vscode", ".idea",
             "node_modules", "__pycache__", ".DS_Store"}
# Frontmatter is a leading `---` ... `---` block. Only scan this many lines into
# a file before giving up looking for the fence / summary — frontmatter is small.
FRONTMATTER_SCAN_LINES = 40


def read_summary(md_path: Path) -> str:
    """Return the `summary:` frontmatter value, or "" if absent.

    Reads ONLY the leading YAML frontmatter block (between the first two `---`
    fences). Never reads the note body. Bounded line scan; tolerant of files
    with no frontmatter at all.
    """
    try:
        with md_path.open("r", errors="replace") as f:
            first = f.readline()
            if first.strip() != "---":
                return ""  # no frontmatter block
            for _ in range(FRONTMATTER_SCAN_LINES):
                line = f.readline()
                if line == "":
                    break  # EOF
                stripped = line.strip()
                if stripped == "---":
                    break  # end of frontmatter
                # Match `summary:` at the start of the line (frontmatter key).
                if stripped.startswith("summary:"):
                    val = stripped[len("summary:"):].strip()
                    # Strip surrounding quotes if present.
                    if len(val) >= 2 and val[0] == val[-1] and val[0] in ("'", '"'):
                        val = val[1:-1]
                    return val.strip()
    except Exception:
        return ""
    return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vault_root")
    ap.add_argument("--max-files-per-dir", type=int, default=200,
                    help="cap notes listed per folder (keeps the digest compact)")
    args = ap.parse_args()

    root = Path(args.vault_root).expanduser().resolve()
    if not root.is_dir():
        print(f"ERROR: vault root is not a directory: {root}", file=sys.stderr)
        return 1

    print(f"# Vault digest — {root}")
    print(f"# (folder tree + per-note `summary:`; no note bodies were read)")
    print()

    for dirpath, dirnames, filenames in os.walk(root):
        # Prune skip dirs + dotfolders in-place so os.walk doesn't descend.
        dirnames[:] = sorted(
            d for d in dirnames
            if d not in SKIP_DIRS and not d.startswith(".")
        )

        md_files = sorted(f for f in filenames if f.lower().endswith(".md"))
        if not md_files and not dirnames:
            continue

        rel = Path(dirpath).relative_to(root)
        # Heading depth tracks folder depth (## for vault root, deeper for nested).
        depth = 0 if str(rel) == "." else len(rel.parts)
        heading = "#" * min(depth + 2, 6)
        label = "/" if str(rel) == "." else str(rel)
        print(f"{heading} {label}")

        if not md_files:
            print()
            continue

        shown = md_files[: args.max_files_per_dir]
        for fn in shown:
            name = fn[:-3] if fn.lower().endswith(".md") else fn
            summary = read_summary(Path(dirpath) / fn)
            if summary:
                print(f"- {name}: {summary}")
            else:
                print(f"- {name}")
        extra = len(md_files) - len(shown)
        if extra > 0:
            print(f"- … (+{extra} more notes)")
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
