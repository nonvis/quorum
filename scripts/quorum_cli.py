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
  quorum where <name> [--repo <name> | --all] [--limit N] [--json]
      Every place an identifier occurs in the committed code: def, import, ref,
      then mentions in comments/strings. Exact case first; a qualified name
      (a::b, A.b) looks up its last part.
  quorum affected [<file>…] [--stdin] [--base <ref>] [--repo <name>] [--quiet | --json]
      The test files a change reaches through imports (Python, TS/JS, C/C++,
      shell) and, for Rust and Move, the packages it reaches through path/local
      deps. Default change set: uncommitted edits + untracked files (+ the
      branch since its merge-base with --base). Files given are relative to the
      current dir; --stdin lines are repo-relative (as `git diff --name-only`
      prints them). --quiet prints test paths only.
  quorum repo index [--repo <name> | --all] [--rebuild]
      Build or refresh repo indexes now. A repo is indexed only if it is on the
      vault's allowlist or has opted in by having a .quorum/ dir.
  quorum hook
      The UserPromptSubmit hook: reads Claude Code's hook JSON on stdin and, when
      the prompt names a project or a Folder/NN note, prints the project's memory
      index + briefing (or that note) as additionalContext (<= 5 hits, once per
      session). Never fails a prompt: any error exits 0 silently; QUORUM_HOOK=off
      disables it. Each call appends a line to <vault>/.quorum/hook.log (session,
      anchor count, hit paths — never the prompt). `hook --replay` scores the
      matcher on past sessions (scripts/hook_replay.py).
  quorum radio <project> | brainstorm <project> "<question>" | inputs <project> …
      One project's day on air, or one question put to the room (scripts/radio.py,
      run as a subprocess): the one verb that calls a model. See scripts/radio.py.
  quorum web start|stop|status|logs|dev
      Radio Studio, the React front over a 127.0.0.1 API (scripts/web_ctl.py).
  quorum study new|list|show|start|say|end|finish|delete …
      Learn one repo ~30 min a day: one tutor session a day, its close the memory the
      next day starts from (scripts/study.py, run as a subprocess; calls a model).
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
RADIO_SCRIPT = REPO / "scripts" / "radio.py"

RETIRED = {"ask", "converse", "knower", "init", "search", "agent", "vault",
           "brainstorm", "autopilot", "run", "status"}

USAGE = """usage: quorum <command> [options]

  spend [--project <path|name>] [--since <ISO8601>] [--until <ISO8601>]
        [--session <id|prefix>] [--json | --line]
                  token/$ readout from Claude Code transcripts ($0)
  q <terms…> [--limit N] [--source vault|memory] [--vault-only] [--json]
                  ranked search over vault notes + memory files, then the
                  allowlisted repos' docs/ + root guides ($0)
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
  where <name> [--repo <name> | --all] [--limit N] [--json]
                  every place an identifier occurs: def, import, ref, text
  affected [<file>…] [--stdin] [--base <ref>] [--repo <name>] [--quiet | --json]
                  the tests a change reaches (default: uncommitted edits)
  repo index [--repo <name> | --all] [--rebuild]
                  build/refresh repo indexes (allowlisted or opted-in repos only)
  lint [--fix-safe] [--json] [--show N]
                  hygiene report: dangling links, orphans, missing summaries, dead
                  file mentions, memory index gaps, repo docs pointing into the
                  vault or at gone files, locked docs missing; --fix-safe repairs
                  only unambiguous link spellings
  hook            UserPromptSubmit hook (stdin JSON -> additionalContext)
  hook on|off|status
                  turn the hook on/off for every session in this vault (a flag in
                  .quorum/); off still logs the prompt's session, so --report can
                  compare. QUORUM_HOOK=off does the same for one shell's sessions.
  hook --report [--since <ISO8601>] [--json]
                  how often the hook injected, how many hits were then Read,
                  how many sessions ran quorum lookups themselves, and per mode
                  (on/off): sessions, prompts, main-session context per turn
  hook --replay [--sessions N] [--show N] [--json]
                  the newest N (40) vault sessions' prompts through today's
                  matcher, scored by the notes each turn then Read ($0)
  radio <project> [today|yesterday|week|YYYY-MM-DD|A..B] [--with operator] [--fake] [--no-voice]
                  the one model-calling verb: one project's day or range (KST)
                  as a spoken recap — a script + an MP3 in bound voices, in the
                  per-host store (.quorum/radio/; unkept ones go after 7 days)
  radio brainstorm <project> "<question>" [--with id,…] [--context <episode id>,…]
                  one question put to the room; the host drives; a script
  web start [--fake] | stop | status | logs [-f] | dev | build
                  Radio Studio on http://127.0.0.1:4747: generate, play, keep,
                  delete, export; manage projects and specialties
  radio inputs <project> [range] [--since …] [--until …]
                  the day's inputs per specialty slice (JSON, $0)
  study new <track> --project <p> --repo <path> [--subtree <dir>] --goal "…"
  study start|say|end|show|finish|delete <track> … | study list
                  learn one repo a day at a time: one Opus tutor session a day
                  (read-only, no git), End saves the memory the next day loads
                  (per-host store .quorum/study/; Radio Studio's Study tab)
  curate [--days N] [--show N] [--json]
                  memory files no session Read or Edited in the last N (30) days,
                  with the bytes their MEMORY*.md rows cost every session ($0;
                  untouched is not stale: the owner decides)
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


def _repo_doc_hits(text, limit=3):
    """Allowlisted repos' docs/ + root guides matching text, as vault-style hits
    (path "repo:<name>:<file>")."""
    sys.path.insert(0, str(REPO / "scripts"))
    import repo_index
    targets, err = _repo_targets({"--all": True})
    out = []
    for name, path in targets or []:
        ri = _open_repo(path)
        for h in ri.search(text, limit=limit * 3):
            if h["kind"] == "doc" and h["docs"]:
                out.append({"path": "repo:%s:%s" % (name, h["ref"]), "title": h["title"],
                            "summary": "", "snippet": h["snippet"], "updated": "",
                            "score": h["score"], "matched": h["matched"]})
        ri.close()
    out.sort(key=lambda h: (-h["matched"], h["score"]))
    return out[:limit]


def cmd_q(args):
    import json
    opts, terms = _flags(args, {"--limit", "--source"}, {"--json", "--vault-only"})
    if not terms:
        print("usage: quorum q <terms…> [--limit N] [--source vault|memory] [--json]",
              file=sys.stderr)
        return 2
    idx = _index()
    idx.refresh()
    hits = idx.search(" ".join(terms), limit=int(opts.get("--limit", 8)),
                      source=opts.get("--source"))
    idx.close()
    docs = [] if (opts.get("--vault-only") or opts.get("--source")) else \
        _repo_doc_hits(" ".join(terms))
    if opts.get("--json"):
        print(json.dumps(hits + docs, ensure_ascii=False, indent=1))
        return 0
    for h in hits:
        print(h["path"])
        line = h["summary"] or h["snippet"]
        if line:
            print("    " + " ".join(line.split())[:220])
    if docs:
        print("— repo docs:")
        for h in docs:
            print(h["path"])
            if h["snippet"]:
                print("    " + " ".join(h["snippet"].split())[:220])
    if not hits and not docs:
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


WHERE_LIMIT = 200  # hits printed in full; the rest collapse to a count per file


def cmd_where(args):
    import json
    opts, rest = _flags(args, {"--repo", "--limit"}, {"--all", "--json"})
    if len(rest) != 1:
        print("usage: quorum where <name> [--repo <name> | --all] [--limit N] [--json]",
              file=sys.stderr)
        return 2
    targets, err = _repo_targets(opts)
    if err:
        print("ERROR: " + err, file=sys.stderr)
        return 1
    limit = int(opts.get("--limit", WHERE_LIMIT)) or None  # 0 = every hit
    if opts.get("--json"):
        limit = None
    hits, exact, keyword_in = [], True, []
    for name, path in targets:
        ri = _open_repo(path)
        r = ri.where(rest[0], limit=limit)
        ri.close()
        exact, keyword_in = exact and r["exact"], r["keyword_in"]
        for h in r["hits"]:
            h["repo"] = name
            hits.append(h)
    roles = ("def", "import", "ref", "text")
    hits.sort(key=lambda h: (roles.index(h["role"]), h["repo"], h["path"], h["line"]))
    if opts.get("--json"):
        print(json.dumps(hits, ensure_ascii=False, indent=1))
        return 0
    if keyword_in:
        print("(%s is a keyword in %s: those files store no occurrence of it)"
              % (rest[0], ", ".join(keyword_in)))
    if not hits:
        print("(no occurrence of %r)" % rest[0], file=sys.stderr)
        return 0
    if not exact:
        print("(no exact-case match; case-insensitive)", file=sys.stderr)
    for h in hits[:limit]:
        print("%s  %-6s %s:%d%s  %s" % (h["repo"], h["role"], h["path"], h["line"],
                                       "*" if h["dirty"] else "", h["text"]))
    rest_hits = hits[limit:] if limit else []
    if rest_hits:
        per_file = {}
        for h in rest_hits:
            per_file[(h["repo"], h["path"])] = per_file.get((h["repo"], h["path"]), 0) + 1
        for (repo, path), n in sorted(per_file.items(), key=lambda kv: -kv[1])[:30]:
            print("%s  + %s: %d more" % (repo, path, n))
        if len(per_file) > 30:
            print("  … and %d more files" % (len(per_file) - 30))
        print("(%d hits not shown: --limit 0 for all, --json for every field)" % len(rest_hits))
    counts = [(sum(1 for h in hits if h["role"] == r), r) for r in ("def", "import", "ref")]
    parts = ["%d %s%s" % (n, r, "" if n == 1 else "s") for n, r in counts if n]
    text = sum(1 for h in hits if h["role"] == "text")
    if text:
        parts.append("%d in comments/strings" % text)
    files = len({(h["repo"], h["path"]) for h in hits})
    parts.append("%d file%s" % (files, "" if files == 1 else "s"))
    print(" · ".join(parts))
    if any(h["dirty"] for h in hits):
        print("* the file has uncommitted edits; line numbers are HEAD's")
    return 0


def cmd_affected(args):
    import json
    opts, rest = _flags(args, {"--repo", "--base"}, {"--stdin", "--quiet", "--json", "--all"})
    if opts.get("--all"):
        print("usage: quorum affected works on one repo", file=sys.stderr)
        return 2
    targets, err = _repo_targets(opts)
    if err:
        print("ERROR: " + err, file=sys.stderr)
        return 1
    if len(targets) != 1:
        print("ERROR: run inside a repo or name one with --repo", file=sys.stderr)
        return 1
    name, path = targets[0]
    ri = _open_repo(path)
    changed = [os.path.abspath(f) for f in rest]
    if opts.get("--stdin"):
        changed += [line.strip() for line in sys.stdin if line.strip()]
    if not rest and not opts.get("--stdin"):
        changed = ri.changed_files(opts.get("--base"))
    r = ri.affected(changed)
    ri.close()
    if opts.get("--json"):
        print(json.dumps(r, ensure_ascii=False, indent=1))
        return 0
    if opts.get("--quiet"):
        for t in r["tests"]:
            print(t["path"])
        return 0
    if not changed:
        print("(no changes)", file=sys.stderr)
        return 0
    for p in r["packages"]:
        print("%s  package %s (%s %s)  ← %s%s" % (
            name, p["path"] or ".", p["kind"], p["name"], p["origin"],
            " (%d hop%s)" % (p["hops"], "" if p["hops"] == 1 else "s") if p["hops"] else ""))
    for t in r["tests"]:
        print("%s  test    %s  ← %s%s" % (
            name, t["path"], t["origin"],
            " (%d hop%s)" % (t["hops"], "" if t["hops"] == 1 else "s") if t["hops"] else ""))
    n, k = len(r["tests"]), len(r["packages"])
    parts = ["%d test%s" % (n, "" if n == 1 else "s")]
    if k:
        parts.append("%d package%s" % (k, "" if k == 1 else "s"))
    parts.append("from %d changed" % len(set(changed)))
    print(" · ".join(parts))
    if r["unreached"]:
        print("reach no test: " + ", ".join(r["unreached"][:12])
              + (" …" if len(r["unreached"]) > 12 else ""))
    return 0


HOOK_MAX_CHARS = 3600


LOOKUP = re.compile(r"\bquorum\s+(?:q|links|mentions|repos|sym|where|affected|repo\s+q)\b")


def hook_report(as_json, since=None):
    """The readout for the hook and the lookup commands, from hook.log and the
    vault's session transcripts (main + subagents): how often the hook fired,
    how many injected notes the session then Read, and how many sessions ran a
    `quorum` lookup themselves. `since` (ISO, UTC) drops the log lines and the
    lookups before it — the readout of a hook changed at that time."""
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
    if since:
        log = [l for l in log if l.get("ts", "") >= since]
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
                    if c.get("name") == "Bash" and LOOKUP.search(inp.get("command", "")) \
                            and r.get("timestamp", "") >= (since or ""):
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


def hook_replay_cmd(args):
    """The newest vault sessions' prompts through today's matcher, scored by the
    notes each turn then Read (scripts/hook_replay.py)."""
    import json
    opts, _ = _flags(args, {"--sessions", "--show"}, {"--replay", "--json"})
    sys.path.insert(0, str(REPO / "scripts"))
    import hook_replay
    idx = _index()
    idx.refresh()
    r = hook_replay.replay(idx, hook_replay.transcript_dir(idx.vault),
                           sessions=int(opts.get("--sessions", 40)))
    idx.close()
    if opts.get("--json"):
        print(json.dumps(r, ensure_ascii=False, indent=1))
        return 0
    s = r["summary"]
    print("replay: %d prompts in %d sessions; %d then Read a note" % (
        s["prompts"], s["sessions"], s["needing"]))
    print("  fired %d, useful %d · hits %d, right %d · precision %s · coverage %s" % (
        s["fired"], s["useful"], s["hits"], s["hits_right"], s["precision"], s["coverage"]))
    show = int(opts.get("--show", 0))
    for t in [t for t in r["turns"] if t["hits"]][-show:] if show else []:
        print("  %s %s" % ("✓" if t["right"] else "✗", " ".join(t["prompt"].split())[:90]))
        for h in t["hits"]:
            print("      %s %s" % ("✓" if h in t["right"] else "·", h.split(":", 1)[1][:100]))
    return 0


def cmd_hook(args):
    """Never raises, never exits non-zero: a hook must not block a prompt."""
    import json
    import time
    if "--report" in args:
        opts, _ = _flags(args, {"--since"}, {"--report", "--json"})
        return hook_report(bool(opts.get("--json")), opts.get("--since"))
    if "--replay" in args:
        return hook_replay_cmd(args)
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
        seen = set()
        try:
            for line in (vault_index.default_vault() / ".quorum" / "hook.log").read_text().splitlines():
                if payload.get("session_id") and '"%s"' % payload["session_id"] in line:
                    seen.update(json.loads(line).get("hits") or [])
        except (OSError, ValueError):
            pass
        idx = _index()
        idx.refresh()
        terms, hits = idx.prompt_hits(prompt, seen=seen)
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


LINT_LABELS = [("dangling_links", "dangling links"), ("ambiguous_links", "ambiguous links"),
               ("orphans", "orphans"),
               ("no_summary", "no summary"), ("dead_mentions", "dead mentions"),
               ("memory_unindexed", "memory unindexed"),
               ("memory_index_broken", "memory index rows → missing file"),
               ("docs_vault_refs", "docs → vault refs"), ("docs_dead_refs", "docs dead refs"),
               ("locked_docs_missing", "locked docs missing")]


def cmd_lint(args):
    import json
    opts, _ = _flags(args, {"--show"}, {"--json", "--fix-safe"})
    sys.path.insert(0, str(REPO / "scripts"))
    import lint
    idx = _index()
    idx.refresh()
    r = lint.run(idx, fix_safe=bool(opts.get("--fix-safe")))
    idx.close()
    if opts.get("--json"):
        print(json.dumps(r, ensure_ascii=False, indent=1))
        return 0
    show = int(opts.get("--show", 5))
    if r["fixed"]:
        print("fixed %d link(s) (--fix-safe)" % r["fixed"])
    for key, label in LINT_LABELS:
        items = r["checks"][key]
        print("%-34s %5d" % (label, len(items)))
        for it in items[:show]:
            print("    " + (it if isinstance(it, str) else
                            " · ".join(str(v) for v in it.values())))
    return 0


def cmd_curate(args):
    import json
    opts, _ = _flags(args, {"--days", "--show"}, {"--json"})
    sys.path.insert(0, str(REPO / "scripts"))
    import curate
    import hook_replay
    import vault_index
    vault = vault_index.default_vault()
    memory = vault_index.default_memory(vault)
    tdir = os.environ.get("QUORUM_TRANSCRIPTS") or hook_replay.transcript_dir(vault)
    r = curate.curate(memory, [tdir] if tdir else [], now=os.environ.get("QUORUM_NOW"),
                      days=int(opts.get("--days", 30)))
    if opts.get("--json"):
        print(json.dumps(r, ensure_ascii=False, indent=1))
        return 0
    w, files = r["window"], r["files"]
    idle = [f for f in files if not f["reads"] + f["edits"]]
    print("window %s → %s (%d d, %d sessions; oldest transcript on this host %s)"
          % (w["from"][:10], w["to"][:10], w["days"], w["sessions"],
             (w["oldest_transcript"] or "none")[:10]))
    print("untouched: %d of %d memory files · rows naming only untouched files: "
          "%d B in MEMORY.md (every session), %d B in MEMORY_<X>.md (when read)"
          % (len(idle), len(files), r["idle_rows"]["auto"], r["idle_rows"]["second_level"]))
    for f in idle[:int(opts.get("--show", 25))]:
        print("  %6d B row  %-58s %s" % (f["row_bytes"], f["file"],
                                          ", ".join(f["indexes"]) or "(no index row)"))
    return 0


def cmd_radio(args):
    """Radio runs in its own process: the core never imports it (QR-1)."""
    return subprocess.call([sys.executable, str(RADIO_SCRIPT)] + list(args))


def cmd_web(args):
    """Radio Studio's start/stop (QR-23), in its own process like radio."""
    return subprocess.call([sys.executable, str(REPO / "scripts" / "web_ctl.py")] + list(args))


def cmd_study(args):
    """Study (QR-41…47) in its own process, like radio: the core never imports it."""
    return subprocess.call([sys.executable, str(REPO / "scripts" / "study.py")] + list(args))


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
    if sub == "where":
        return cmd_where(rest)
    if sub == "affected":
        return cmd_affected(rest)
    if sub == "hook":
        return cmd_hook(rest)
    if sub == "lint":
        return cmd_lint(rest)
    if sub == "curate":
        return cmd_curate(rest)
    if sub == "radio":
        return cmd_radio(rest)
    if sub == "web":
        return cmd_web(rest)
    if sub == "study":
        return cmd_study(rest)
    if sub in RETIRED:
        print("ERROR: `quorum %s` was retired with the C++ daemon on 2026-10-16 "
              "(vault Quorum Decision Log #98)." % sub, file=sys.stderr)
        return 2
    print("ERROR: unknown command %r\n\n%s" % (sub, USAGE), file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
