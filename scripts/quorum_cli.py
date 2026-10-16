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
  quorum hook
      The UserPromptSubmit hook: reads Claude Code's hook JSON on stdin and, when
      the prompt clearly matches vault notes, prints their paths + summaries as
      additionalContext (<= 5 hits, ~1k tokens). Never fails a prompt: any error
      exits 0 silently; QUORUM_HOOK=off disables it. Each call appends a line to
      <vault>/.quorum/hook.log (session, term count, hit paths — never the prompt).
  quorum version

`~/.local/bin/quorum` is a symlink to this file (scripts/install.sh).
"""
import os
import re
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
  hook            UserPromptSubmit hook (stdin JSON -> additionalContext)
  hook on|off|status
                  turn the hook on/off for every session in this vault (a flag in
                  .quorum/); off still logs the prompt's session, so --report can
                  compare. QUORUM_HOOK=off does the same for one shell's sessions.
  hook --report [--json]
                  how often the hook injected, how many hits were then Read,
                  how many sessions ran quorum lookups themselves, and per mode
                  (on/off): sessions, prompts, main-session context per turn
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


HOOK_MAX_CHARS = 3600


LOOKUP = re.compile(r"\bquorum\s+(?:q|links|mentions|repos|sym|repo\s+q)\b")


def hook_report(as_json):
    """The readout for the hook and the lookup commands, from hook.log and the
    vault's session transcripts (main + subagents): how often the hook fired,
    how many injected notes the session then Read, and how many sessions ran a
    `quorum` lookup themselves."""
    import json
    sys.path.insert(0, str(REPO / "scripts"))
    import vault_index
    vault = vault_index.default_vault()
    memory = vault_index.default_memory(vault)
    log = []
    try:
        for line in (vault / ".quorum" / "hook.log").read_text().splitlines():
            try:
                log.append(json.loads(line))
            except ValueError:
                pass
    except OSError:
        pass
    # Claude Code names the dir from the path as the session saw it, which may
    # differ from the resolved one (macOS: /var -> /private/var).
    tdir = None
    for v in (os.environ.get("QUORUM_VAULT") or "", str(vault)):
        if v:
            cand = Path(os.path.expanduser("~/.claude/projects")) / "".join(
                "-" if c in "/." else c for c in os.path.expanduser(v))
            if cand.is_dir():
                tdir = cand
                break
    tdir = tdir or Path("/nonexistent")
    sessions = sorted({l["session"] for l in log if l.get("session")})
    reads, lookups = {}, {}
    for sid in sessions:
        files = [tdir / ("%s.jsonl" % sid)] + sorted((tdir / sid / "subagents").glob("*.jsonl"))
        rs, n = [], 0
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
                content = (r.get("message") or {}).get("content")
                if not isinstance(content, list):
                    continue
                for c in content:
                    if not isinstance(c, dict) or c.get("type") != "tool_use":
                        continue
                    inp = c.get("input") or {}
                    if c.get("name") == "Read" and inp.get("file_path"):
                        rs.append((r.get("timestamp", ""), os.path.realpath(inp["file_path"])))
                    if c.get("name") == "Bash" and LOOKUP.search(inp.get("command", "")):
                        n += 1
        reads[sid], lookups[sid] = rs, n

    def real(hit):
        src, rel = hit.split(":", 1)
        return os.path.realpath(str((vault if src == "vault" else memory) / rel))

    def main_context(sid):
        """(main-transcript turns, their cache-write + cache-read tokens),
        deduplicated by message id — the context-per-turn quorum spend reads."""
        seen = {}
        try:
            lines = (tdir / ("%s.jsonl" % sid)).read_text(errors="replace").splitlines()
        except OSError:
            return 0, 0
        for line in lines:
            try:
                r = json.loads(line)
            except ValueError:
                continue
            m = r.get("message") or {}
            u = m.get("usage") if isinstance(m, dict) else None
            if r.get("type") == "assistant" and isinstance(u, dict) and m.get("id"):
                seen[m["id"]] = (u.get("cache_creation_input_tokens") or 0) + \
                    (u.get("cache_read_input_tokens") or 0)
        return len(seen), sum(seen.values())

    modes = {}
    for l in log:
        if l.get("session") and l["session"] not in modes:
            modes[l["session"]] = l.get("mode", "on")
    by_mode = {}
    for mode in ("on", "off"):
        sids = [s for s, m in modes.items() if m == mode]
        turns = ctx = 0
        for sid in sids:
            t, c = main_context(sid)
            turns += t
            ctx += c
        by_mode[mode] = {"sessions": len(sids),
                         "prompts": sum(1 for l in log if modes.get(l.get("session")) == mode),
                         "turns": turns,
                         "context_per_turn": round(ctx / turns) if turns else None}

    hits = hits_read = injected = 0
    for l in log:
        if not l.get("hits"):
            continue
        injected += 1
        for h in l["hits"]:
            hits += 1
            target = real(h)
            if any(ts >= l["ts"] and p == target for ts, p in reads.get(l.get("session"), [])):
                hits_read += 1
    out = {"calls": len(log), "injected": injected, "hits": hits, "hits_read": hits_read,
           "sessions": len(sessions),
           "sessions_with_lookups": sum(1 for v in lookups.values() if v),
           "lookup_calls": sum(lookups.values()),
           "by_mode": by_mode,
           "since": log[0]["ts"] if log else None}
    if as_json:
        print(json.dumps(out, indent=1))
    else:
        print("hook since %s: %d prompts, %d injected (%d notes), %d of them then Read; "
              "%d of %d sessions ran a quorum lookup themselves (%d calls)" % (
                  out["since"], out["calls"], injected, hits, hits_read,
                  out["sessions_with_lookups"], out["sessions"], out["lookup_calls"]))
        for mode, m in by_mode.items():
            print("  hook %-3s: %d sessions, %d prompts, main context/turn %s" % (
                mode, m["sessions"], m["prompts"],
                "~%dk" % round(m["context_per_turn"] / 1000) if m["context_per_turn"]
                else "n/a"))
    return 0


def cmd_hook(args):
    """Never raises, never exits non-zero: a hook must not block a prompt."""
    import json
    import time
    if "--report" in args:
        return hook_report("--json" in args)
    sys.path.insert(0, str(REPO / "scripts"))
    import vault_index
    flag = vault_index.default_vault() / ".quorum" / "hook-off"
    if args and args[0] in ("on", "off", "status"):
        if not vault_index.default_vault().is_dir():
            print("ERROR: no vault at %s" % vault_index.default_vault(), file=sys.stderr)
            return 1
        if args[0] == "off":
            flag.parent.mkdir(exist_ok=True)
            flag.touch()
        elif args[0] == "on" and flag.exists():
            flag.unlink()
        print("hook: %s%s" % ("off" if flag.exists() else "on",
                               "  (QUORUM_HOOK=off in this shell)" if
                               os.environ.get("QUORUM_HOOK", "").lower() in ("off", "0", "no")
                               else ""))
        return 0
    try:
        if sys.stdin.isatty():
            return 0
        payload = json.loads(sys.stdin.read() or "{}")
        prompt = payload.get("prompt") or ""
        if not vault_index.default_vault().is_dir():
            return 0
        if os.environ.get("QUORUM_HOOK", "").lower() in ("off", "0", "no") or flag.exists():
            try:
                with open(flag.parent / "hook.log", "a") as f:
                    f.write(json.dumps({
                        "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                        "session": payload.get("session_id"), "mode": "off",
                        "terms": 0, "hits": [], "chars": 0}) + "\n")
            except OSError:
                pass
            return 0
        idx = _index()
        idx.refresh()
        terms, hits = idx.prompt_hits(prompt)
        lines = []
        for h in hits:
            summary = " ".join((h["summary"] or h["snippet"] or "").split())[:180]
            lines.append("- %s%s" % (h["path"], " — " + summary if summary else ""))
        text = ""
        if lines:
            text = ("Vault index (quorum q) — notes that match this prompt; read one "
                    "only if it helps:\n" + "\n".join(lines))
            while len(text) > HOOK_MAX_CHARS and lines:
                lines.pop()
                text = text.split("\n")[0] + "\n" + "\n".join(lines)
        try:
            with open(idx.vault / ".quorum" / "hook.log", "a") as f:
                f.write(json.dumps({
                    "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                    "session": payload.get("session_id"), "mode": "on",
                    "terms": len(terms),
                    "hits": [h["path"] for h in hits[:len(lines)]],
                    "chars": len(text)}) + "\n")
        except OSError:
            pass
        idx.close()
        if text and lines:
            print(json.dumps({"hookSpecificOutput": {
                "hookEventName": "UserPromptSubmit", "additionalContext": text}}))
    except Exception:
        pass
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
    if sub == "hook":
        return cmd_hook(rest)
    if sub in RETIRED:
        print("ERROR: `quorum %s` was retired with the C++ daemon on 2026-10-16 "
              "(vault Quorum Decision Log #98)." % sub, file=sys.stderr)
        return 2
    print("ERROR: unknown command %r\n\n%s" % (sub, USAGE), file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
