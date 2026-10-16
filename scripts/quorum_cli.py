#!/usr/bin/env python3
"""quorum — the command-line entry point (Python, stdlib only).

Quorum was a C++ daemon that ran Claude Code agents; that daemon, its web app
and its agent model were retired on 2026-10-16. What remains is deterministic
tooling that spends no model tokens:

  quorum spend [--project <path|name>] [--since <ISO8601>] [--until <ISO8601>]
               [--session <id|prefix>] [--json | --line]
      Token/$ readout from the Claude Code transcripts (scripts/spend_readout.py).
  quorum q <terms…> [--limit N] [--source vault|memory] [--json]
      Ranked search over the vault's notes and memory files (scripts/vault_index.py).
  quorum links <note> [--json]
      A note's outgoing [[links]] (resolved or dangling) and its backlinks.
  quorum mentions <repo|~/path|file|sha> [--json]
      The notes that name a repo, a file in it, or a commit.
  quorum repos [--json]
      Every repo the vault's notes name: resolved by git (worktrees folded in),
      not-git, or absent on this host; origin's URL; on the allowlist or not.
  quorum index [--stats] [--rebuild]
      Refresh the index now (every read already refreshes first).
  quorum repo q <terms…> [--repo <name> | --all] [--limit N] [--json]
      Search a repo's own index (<repo>/.quorum/index.db): markdown, commit
      messages, symbol names. Default: the repo you are in; outside one, --all.
  quorum sym <name> [--repo <name> | --all] [--json]
      Where a symbol is defined (case-insensitive prefix).
  quorum repo index [--repo <name> | --all] [--rebuild]
      Build or refresh repo indexes now. A repo is indexed only if it is on the
      vault's allowlist or has opted in by having a .quorum/ dir.
  quorum version

`~/.local/bin/quorum` is a symlink to this file (scripts/install.sh).
"""
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SPEND_SCRIPT = REPO / "scripts" / "spend_readout.py"

RETIRED = {"ask", "converse", "knower", "init", "search", "agent", "vault",
           "brainstorm", "autopilot", "run", "status"}

USAGE = """usage: quorum <command> [options]

  spend [--project <path|name>] [--since <ISO8601>] [--until <ISO8601>]
        [--session <id|prefix>] [--json | --line]
                  token/$ readout from Claude Code transcripts ($0)
  q <terms…> [--limit N] [--source vault|memory] [--json]
                  ranked search over vault notes + memory files ($0)
  links <note> [--json]
                  outgoing [[links]] and backlinks of one note
  mentions <repo|~/path|file|sha> [--json]
                  notes that name a repo, a file or a commit
  repos [--json]  repos the notes name, resolved by git, with the allowlist
  index [--stats] [--rebuild]
                  refresh the vault index (every read refreshes on its own)
  repo q <terms…> [--repo <name> | --all] [--limit N] [--json]
                  search one repo's docs, commit messages and symbols
  sym <name> [--repo <name> | --all] [--json]
                  where a symbol is defined
  repo index [--repo <name> | --all] [--rebuild]
                  build/refresh repo indexes (allowlisted or opted-in repos only)
  version         print the checkout's commit

The vault defaults to ~/nonvis/second-brain (QUORUM_VAULT overrides); its memory
dir is Claude Code's for that path (QUORUM_MEMORY overrides).
"""


def resolve_project(arg):
    """A directory as given (a leading ~ expanded), else a project NAME looked up
    under ~/projects then ~/nonvis — the first holding a .quorum/ wins."""
    home = os.environ.get("HOME", "")
    given = home + arg[1:] if arg.startswith("~") and home else arg
    if os.path.isdir(given):
        return os.path.abspath(given), None
    tried = []
    for root in ("projects", "nonvis"):
        cand = os.path.join(home, root, arg)
        if os.path.isdir(os.path.join(cand, ".quorum")):
            return cand, None
        tried.append(cand)
    return None, ("cannot resolve project %r: not a directory, and no .quorum/ in %s"
                  % (arg, ", ".join(tried)))


def cmd_spend(args):
    opts = {"--project": None, "--since": None, "--until": None, "--session": None}
    flags = []
    i = 0
    while i < len(args):
        a = args[i]
        if a in opts and i + 1 < len(args):
            opts[a] = args[i + 1]
            i += 2
        elif a in ("--json", "--line"):
            flags.append(a)
            i += 1
        else:
            print("ERROR: unknown spend argument %r\n\n%s" % (a, USAGE), file=sys.stderr)
            return 2
    project, err = resolve_project(opts["--project"] or ".")
    if err:
        print("ERROR: " + err, file=sys.stderr)
        return 1
    script = os.environ.get("QUORUM_SPEND_SCRIPT") or str(SPEND_SCRIPT)
    cmd = [sys.executable, script, "--project", project]
    for k in ("--since", "--session", "--until"):
        if opts[k]:
            cmd += [k, opts[k]]
    cmd += flags
    return subprocess.call(cmd)


def _index():
    sys.path.insert(0, str(REPO / "scripts"))
    import vault_index
    return vault_index.open_default()


def _flags(args, valued, bools):
    opts, rest, i = {}, [], 0
    while i < len(args):
        a = args[i]
        if a in valued and i + 1 < len(args):
            opts[a] = args[i + 1]
            i += 2
        elif a in bools:
            opts[a] = True
            i += 1
        else:
            rest.append(a)
            i += 1
    return opts, rest


def cmd_q(args):
    import json
    opts, terms = _flags(args, {"--limit", "--source"}, {"--json"})
    if not terms:
        print("usage: quorum q <terms…> [--limit N] [--source vault|memory] [--json]",
              file=sys.stderr)
        return 2
    idx = _index()
    idx.refresh()
    hits = idx.search(" ".join(terms), limit=int(opts.get("--limit", 8)),
                      source=opts.get("--source"))
    if opts.get("--json"):
        print(json.dumps(hits, ensure_ascii=False, indent=1))
        return 0
    for h in hits:
        print(h["path"])
        line = h["summary"] or h["snippet"]
        if line:
            print("    " + " ".join(line.split())[:220])
    if not hits:
        print("(no match)", file=sys.stderr)
    return 0


def cmd_links(args):
    import json
    opts, rest = _flags(args, set(), {"--json"})
    if len(rest) != 1:
        print("usage: quorum links <note> [--json]", file=sys.stderr)
        return 2
    idx = _index()
    idx.refresh()
    r = idx.links(rest[0])
    if r is None:
        print("ERROR: no note named %r in the index" % rest[0], file=sys.stderr)
        return 1
    if opts.get("--json"):
        print(json.dumps(r, ensure_ascii=False, indent=1))
        return 0
    print(r["path"])
    print("  outgoing (%d):" % len(r["outgoing"]))
    for o in r["outgoing"]:
        print("    %s" % (o["path"] or "[[%s]]  (dangling)" % o["target"]))
    print("  backlinks (%d):" % len(r["backlinks"]))
    for b in r["backlinks"]:
        print("    " + b["path"])
    ms = idx.note_mentions(rest[0]) or []
    print("  mentions (%d):" % len(ms))
    for m in ms:
        v = m["root"] + ("/" + m["value"] if m["root"] and m["value"] else "") \
            if m["kind"] == "path" else m["value"]
        print("    %-5s %s" % (m["kind"], v))
    return 0


def cmd_mentions(args):
    import json
    opts, rest = _flags(args, set(), {"--json"})
    if len(rest) != 1:
        print("usage: quorum mentions <repo|~/path|file|sha> [--json]", file=sys.stderr)
        return 2
    idx = _index()
    idx.refresh()
    hits = idx.mentions(rest[0])
    if opts.get("--json"):
        print(json.dumps(hits, ensure_ascii=False, indent=1))
        return 0
    for h in hits:
        print("%s\n    %s%s" % (h["path"], h["value"],
                                "  (%s)" % h["repo"] if h["repo"] else ""))
    if not hits:
        print("(no note mentions %r)" % rest[0], file=sys.stderr)
    return 0


def cmd_repos(args):
    import json
    opts, _ = _flags(args, set(), {"--json"})
    idx = _index()
    idx.refresh()
    rs = idx.repos()
    if opts.get("--json"):
        print(json.dumps(rs, ensure_ascii=False, indent=1))
        return 0
    for r in rs:
        print("%-8s %-4s %-28s %3d notes  %s" % (
            r["status"], "ALLOW" if r["allowed"] else "", r["name"][:28], r["notes"],
            r["path"] + ("  " + r["remote"] if r["remote"] else "")))
    return 0


def cmd_index(args):
    opts, _ = _flags(args, set(), {"--stats", "--rebuild"})
    idx = _index()
    r = idx.refresh(rebuild=bool(opts.get("--rebuild")))
    st = idx.stats()
    print("index %s: vault %d · memory %d · links %d (this refresh: %d changed, %d removed)"
          % (idx.vault / ".quorum" / "index.db", st["vault"], st["memory"], st["links"],
             r["changed"], r["removed"]))
    return 0


def _repo_targets(opts):
    """-> (list of (name, Path), error). Allowlisted repos come from the vault;
    the repo you are in counts if allowlisted or opted in (has .quorum/)."""
    sys.path.insert(0, str(REPO / "scripts"))
    import repo_index
    allowed = {}
    vault_ok = False
    import vault_index
    if vault_index.default_vault().is_dir():
        idx = _index()
        vault_ok = True
        idx.refresh()
        for r in idx.repos():
            if r["allowed"]:
                allowed[r["name"]] = Path(os.path.expanduser(r["path"]))
        idx.close()
    if opts.get("--all"):
        if not vault_ok:
            return None, "--all needs the vault (QUORUM_VAULT) for its allowlist"
        return sorted(allowed.items()), None
    if opts.get("--repo"):
        name = opts["--repo"]
        if name in allowed:
            return [(name, allowed[name])], None
        return None, "repo %r is not on the vault's allowlist (06_Metadata/quorum-repos.txt)" % name
    top = repo_index.toplevel(os.getcwd())
    if top is None:
        if not vault_ok:
            return None, "not inside a git repo, and no vault for --all"
        return sorted(allowed.items()), None
    if top.name in allowed or (top / ".quorum").is_dir():
        return [(top.name, top)], None
    return None, ("%s is not opted in: it is not on the vault's allowlist and has no "
                  ".quorum/ dir (mkdir .quorum to opt in)" % top)


def _open_repo(path):
    sys.path.insert(0, str(REPO / "scripts"))
    import repo_index
    ri = repo_index.RepoIndex(path)
    ri.refresh()
    return ri


def cmd_repo(args):
    import json
    if not args or args[0] not in ("q", "index"):
        print("usage: quorum repo q <terms…> | quorum repo index [...]", file=sys.stderr)
        return 2
    sub, args = args[0], args[1:]
    opts, rest = _flags(args, {"--repo", "--limit"}, {"--all", "--json", "--rebuild"})
    targets, err = _repo_targets(opts)
    if err:
        print("ERROR: " + err, file=sys.stderr)
        return 1
    if sub == "index":
        for name, path in targets:
            sys.path.insert(0, str(REPO / "scripts"))
            import repo_index
            ri = repo_index.RepoIndex(path)
            r = ri.refresh(rebuild=bool(opts.get("--rebuild")))
            st = ri.stats()
            print("%-16s head %s · files %d (docs %d) · symbols %d · commits %d"
                  " (this refresh: %d files, %d commits)" % (
                      name, st["head"], st["files"], st["docs"], st["symbols"],
                      st["commits"], r["files"], r["commits"]))
            ri.close()
        return 0
    if not rest:
        print("usage: quorum repo q <terms…> [--repo <name> | --all]", file=sys.stderr)
        return 2
    limit = int(opts.get("--limit", 10))
    hits = []
    for name, path in targets:
        ri = _open_repo(path)
        for h in ri.search(" ".join(rest), limit=limit):
            h["repo"] = name
            hits.append(h)
        ri.close()
    hits.sort(key=lambda h: (-h["matched"], h["score"]))
    hits = hits[:limit]
    if opts.get("--json"):
        print(json.dumps(hits, ensure_ascii=False, indent=1))
        return 0
    for h in hits:
        print("%s  %-6s %s  %s" % (h["repo"], h["kind"], h["ref"], h["title"]))
        if h["snippet"]:
            print("    " + " ".join(h["snippet"].split())[:200])
    if not hits:
        print("(no match)", file=sys.stderr)
    return 0


def cmd_sym(args):
    import json
    opts, rest = _flags(args, {"--repo"}, {"--all", "--json"})
    if len(rest) != 1:
        print("usage: quorum sym <name> [--repo <name> | --all] [--json]", file=sys.stderr)
        return 2
    targets, err = _repo_targets(opts)
    if err:
        print("ERROR: " + err, file=sys.stderr)
        return 1
    out = []
    for name, path in targets:
        ri = _open_repo(path)
        for s in ri.symbol(rest[0]):
            s["repo"] = name
            out.append(s)
        ri.close()
    if opts.get("--json"):
        print(json.dumps(out, ensure_ascii=False, indent=1))
        return 0
    for s in out:
        print("%s  %s:%d  %s %s" % (s["repo"], s["path"], s["line"], s["kind"], s["name"]))
    if not out:
        print("(no symbol %r)" % rest[0], file=sys.stderr)
    return 0


def cmd_version(_args):
    sha = subprocess.run(["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"],
                         capture_output=True, text=True).stdout.strip() or "unknown"
    print("quorum (%s) python %s" % (sha, sys.version.split()[0]))
    return 0


def main(argv):
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(USAGE, file=sys.stderr)
        return 2 if not argv else 0
    sub, rest = argv[0], argv[1:]
    if sub == "spend":
        return cmd_spend(rest)
    if sub == "version":
        return cmd_version(rest)
    if sub == "q":
        return cmd_q(rest)
    if sub == "links":
        return cmd_links(rest)
    if sub == "index":
        return cmd_index(rest)
    if sub == "mentions":
        return cmd_mentions(rest)
    if sub == "repos":
        return cmd_repos(rest)
    if sub == "repo":
        return cmd_repo(rest)
    if sub == "sym":
        return cmd_sym(rest)
    if sub in RETIRED:
        print("ERROR: `quorum %s` was retired with the C++ daemon on 2026-10-16 "
              "(vault Quorum Decision Log #98)." % sub, file=sys.stderr)
        return 2
    print("ERROR: unknown command %r\n\n%s" % (sub, USAGE), file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
