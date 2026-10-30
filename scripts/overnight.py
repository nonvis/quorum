#!/usr/bin/env python3
"""`quorum overnight <project>` — one owner-started night of vault work (vault Quorum Decision #128).

The night itself is a vault note, the overnight run prompt (named by `06_Metadata/quorum-projects.json`
→ `overnight.run_prompt`): walls, lanes, the morning report's shape, and one `## Project: <id>`
appendix per project. This verb only STARTS it: ONE `claude -p` session (Opus) run from the vault, so the
vault's CLAUDE.md, hooks, allow rules and memory apply exactly as in a session the owner opens by hand;
the prompt names the note and the appendix and nothing else. Nothing schedules it: the owner runs it on
the evening of a day spent on that project, or not at all.

What it keeps, per host, in `.quorum/overnight/<project>/<YYYY-MM-DD>.json`: the session id, start/end,
status, cost (the model's own total), the inbox drafts and the morning report the session wrote, and
the spend line. One night per project per KST day: a done night refuses a second run; a failed or
timed-out one may be rerun. `quorum overnight score` is $0: it reads the vault's overnight ledger note
(`overnight.ledger`), where the morning session records Keep / Discard per draft, and prints kept ÷
drafted per project and lane — the number Decision #127 asked for.

Usage:
  quorum overnight <project> [--dry-run] [--events]     run tonight's night for one project
  quorum overnight list                                 this host's nights, newest first
  quorum overnight score [project]                      kept ÷ drafted from the vault ledger ($0)
Env: QUORUM_VAULT (default ~/nonvis/second-brain) · QUORUM_OVERNIGHT_MODEL · QUORUM_CLAUDE ·
     QUORUM_OVERNIGHT_FAKE=1 (the fake runner, no model call) · QUORUM_OVERNIGHT_STORE · QUORUM_OVERNIGHT_NOW
Exit: 0 ok · 1 refused or failed (the reason on stderr) · 2 usage
"""
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import uuid
from datetime import datetime, timedelta, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import radio_projects  # noqa: E402

REPO = HERE.parent
MODEL = os.environ.get("QUORUM_OVERNIGHT_MODEL", "claude-opus-5-5")
EFFORT = "xhigh"                  # was read from the user's settings, which no longer load
BUDGET_USD = "60"               # the model stops itself here; a night has cost $10–30 list
TIMEOUT_S = 3 * 3600              # the run prompt's own STOP is earlier; this is the kill
KST = timezone(timedelta(hours=9))

# What a night may do: read, search, write drafts and the report, commit by pathspec, look things up, read the web,
# run lanes as subagents. What it may never do, whatever the prompt says: touch a host, a deployment, a profile,
# a container, a cloud CLI, or move a repo's working tree.
ALLOWED = [
    "Read", "Grep", "Glob", "Write", "Edit", "Agent", "WebFetch", "WebSearch",
    "Bash(git add:*)", "Bash(git commit:*)", "Bash(git push:*)", "Bash(git pull:*)", "Bash(git log:*)",
    "Bash(git show:*)", "Bash(git status:*)", "Bash(git diff:*)", "Bash(git rev-parse:*)",
    "Bash(quorum:*)", "Bash(curl:*)", "Bash(python3:*)", "Bash(pdftotext:*)", "Bash(date:*)", "Bash(ls:*)",
    "Bash(wc:*)", "Bash(grep:*)", "Bash(find:*)", "Bash(head:*)", "Bash(tail:*)", "Bash(cat:*)", "Bash(mkdir:*)",
    "Bash(awk:*)", "Bash(sed:*)", "Bash(sort:*)", "Bash(uniq:*)", "Bash(cut:*)", "Bash(echo:*)", "Bash(cd:*)",
    "Bash(hostname:*)", "Bash(file:*)", "Bash(unzip:*)",
]
DENIED = [
    "Bash(gcloud:*)", "Bash(aws:*)", "Bash(docker:*)", "Bash(ssh:*)", "Bash(scp:*)", "Bash(rsync:*)",
    "Bash(box.sh:*)", "Bash(./box.sh:*)", "Bash(pf:*)", "Bash(mer:*)", "Bash(sui:*)", "Bash(walrus:*)",
    "Bash(git checkout:*)", "Bash(git stash:*)", "Bash(git reset:*)", "Bash(git rebase:*)", "Bash(git clean:*)",
    "Bash(git switch:*)", "Bash(rm:*)", "Bash(sudo:*)", "Bash(systemctl:*)", "Bash(launchctl:*)", "Bash(make:*)",
    "Bash(quorum overnight:*)", "Bash(quorum radio:*)", "Bash(quorum web:*)",
]

REPO_ROOTS = ("~/nonvis", "~/projects")


def repo_allows(p, roots=REPO_ROOTS):
    """Read-only verbs in the project's own repos, by exact path: `git -C <repo> log|show|status|grep`. Never a
    bare `git -C:*`, which would let `git -C <repo> checkout` past DENIED."""
    out = []
    for repo in p.get("repos") or []:
        for root in roots:
            d = Path(os.path.expanduser(root)) / repo
            if (d / ".git").exists():
                out += ["Bash(git -C %s %s:*)" % (d, v) for v in ("log", "show", "status", "grep")]
                break
    return out


def vault_allows(vault):
    """The vault's own read verbs by its exact path, `git -C <vault> status|log|show|diff`: night 2 was refused
    `git -C <vault> status` four times. Never a bare `git -C:*` (see repo_allows)."""
    return ["Bash(git -C %s %s:*)" % (os.path.abspath(vault), v) for v in ("status", "log", "show", "diff")]


class OvernightError(Exception):
    pass


# ── time ────────────────────────────────────────────────────────────────────

def kst_date(now):
    return now.astimezone(KST).strftime("%Y-%m-%d")


def kst_midnight(now):
    d = now.astimezone(KST)
    return d.replace(hour=0, minute=0, second=0, microsecond=0)


def default_vault():
    return Path(os.environ.get("QUORUM_VAULT") or os.path.expanduser("~/nonvis/second-brain"))


# ── the plan: project → note + appendix ─────────────────────────────────────

def plan(vault, pid):
    """The project's entry in the projects file, the run prompt note and its `## Project: <pid>` appendix."""
    vault = Path(vault)
    try:
        data = radio_projects.read_file(vault)
    except radio_projects.ProjectsError as e:
        raise OvernightError(str(e))
    cfg = (data.get("projects") or {}).get(pid)
    if not cfg:
        raise OvernightError("no project %r in %s (ids: %s)" % (pid, radio_projects.PROJECTS_REL,
                                                             ", ".join(sorted(data.get("projects") or {}))))
    if not cfg.get("active", True):
        raise OvernightError("project %r is inactive in %s" % (pid, radio_projects.PROJECTS_REL))
    ov = data.get("overnight") or {}
    rel = ov.get("run_prompt")
    if not rel or not radio_projects._safe(rel):
        raise OvernightError("%s has no `overnight.run_prompt` (the vault note that IS the night)" % radio_projects.PROJECTS_REL)
    path = vault / rel
    if not path.is_file():
        raise OvernightError("the overnight run prompt is missing: %s" % path)
    heading = "## Project: %s" % pid
    if not re.search(r"(?m)^%s(\s|$)" % re.escape(heading), path.read_text(encoding="utf-8")):
        raise OvernightError("no appendix `%s` in %s — write the project's walls and gap sources there first" % (heading, rel))
    return {"id": pid, "name": cfg.get("name") or pid, "folders": list(cfg.get("folders") or []),
            "repos": list(cfg.get("repos") or []), "run_prompt": rel, "run_prompt_path": path,
            "ledger": ov.get("ledger") or "", "appendix": heading}


def user_prompt(p, sid, now, scratch=None):
    return ("Run the overnight for project `%s` (%s).\n"
            "Read first and follow exactly: `%s`. Its section `%s` is tonight's walls and gap sources; "
            "no other project's section applies tonight.\n"
            "Start: %s KST. Session id: %s. Host: %s.%s\n"
            "Stop when the morning report is committed and pushed, or at the note's STOP time, whichever is first."
            % (p["id"], p["name"], p["run_prompt"], p["appendix"], now.astimezone(KST).strftime("%Y-%m-%d %H:%M"),
               sid, socket.gethostname(), (" Scratch: %s" % scratch) if scratch else ""))


def commits_since(vault, folders, since_iso):
    """How many vault commits touched the project's folders since `since_iso` — a hint, never a gate."""
    if not folders:
        return 0
    r = subprocess.run(["git", "-C", str(vault), "log", "--since=%s" % since_iso, "--format=%h", "--"] + list(folders),
                       capture_output=True, text=True)
    if r.returncode != 0:
        return 0
    return len([l for l in r.stdout.splitlines() if l.strip()])


# ── the store ───────────────────────────────────────────────────────────────

class Store:
    def __init__(self, root=None):
        self.root = Path(root or os.environ.get("QUORUM_OVERNIGHT_STORE") or (REPO / ".quorum" / "overnight"))

    def path(self, pid, date):
        return self.root / pid / ("%s.json" % date)

    def scratch(self, pid, date):
        """The night's own scratch (walls.md, one subdir per lane): made before the session, removed after a done
        night, kept after a failed one for the rerun."""
        return self.root / pid / ("%s-scratch" % date)

    def save(self, row):
        p = self.path(row["project"], row["date"])
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = p.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(row, indent=1, ensure_ascii=False) + "\n")
        os.replace(tmp, p)

    def night(self, pid, date):
        p = self.path(pid, date)
        return json.loads(p.read_text()) if p.is_file() else None

    def nights(self, pid=None):
        rows = []
        if not self.root.is_dir():
            return rows
        for d in self.root.iterdir():
            if not d.is_dir() or (pid and d.name != pid):
                continue
            for f in d.glob("*.json"):
                try:
                    rows.append(json.loads(f.read_text()))
                except ValueError:
                    continue
        rows.sort(key=lambda r: (r.get("date", ""), r.get("project", "")), reverse=True)
        return rows


# ── the runner ──────────────────────────────────────────────────────────────

def _target(inp):
    inp = inp or {}
    return str(inp.get("file_path") or inp.get("pattern") or inp.get("path") or inp.get("command") or
               inp.get("url") or inp.get("description") or "")[:200]


class Runner:
    """ONE `claude -p` from the vault's own cwd (its CLAUDE.md, hooks, allow rules and memory apply), the
    session id chosen here so `quorum spend --session` can meter it. Streams the tool calls as they happen."""

    def __init__(self, model=MODEL, timeout=TIMEOUT_S, budget=BUDGET_USD):
        self.claude = os.environ.get("QUORUM_CLAUDE", "claude")
        self.model, self.timeout, self.budget = model, timeout, budget

    def command(self, p, vault, sid, scratch=None):
        # Project source only: a host's settings.local.json (bare "Bash" on the MSI) and the user's settings.json
        # (bare "Bash", "Write", "Edit" on the Mac) would outrank ALLOWED. The scratch is the one added dir: never a
        # repo, whose edits acceptEdits would then accept.
        return [self.claude, "-p", "--model", self.model, "--effort", EFFORT, "--session-id", sid,
                "--setting-sources", "project",
                "--permission-mode", "acceptEdits",
                "--allowedTools", ",".join(ALLOWED + vault_allows(vault) + repo_allows(p)),
                "--disallowedTools", ",".join(DENIED)] + (["--add-dir", str(scratch)] if scratch else []) + [
                "--max-budget-usd", self.budget, "--strict-mcp-config", "--output-format", "stream-json", "--verbose"]

    def __call__(self, p, vault, sid, prompt, on_event=None, scratch=None):
        try:
            proc = subprocess.Popen(self.command(p, vault, sid, scratch), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True, cwd=str(vault))
        except OSError as e:
            return {"status": "error", "error": "claude -p could not start: %s" % e, "cost": 0.0, "tools": [], "result": ""}
        proc.stdin.write(prompt)
        proc.stdin.close()
        result, tools = None, []
        try:
            for line in proc.stdout:
                try:
                    o = json.loads(line)
                except ValueError:
                    continue
                if o.get("type") == "assistant":
                    for c in (o.get("message") or {}).get("content") or []:
                        if isinstance(c, dict) and c.get("type") == "tool_use":
                            t = {"name": c.get("name"), "target": _target(c.get("input"))}
                            tools.append(t)
                            if on_event:
                                on_event(dict(event="tool", **t))
                elif o.get("type") == "result":
                    result = o
            proc.wait(timeout=self.timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            return {"status": "timed-out", "error": "claude -p killed after %ds" % self.timeout,
                    "cost": float((result or {}).get("total_cost_usd") or 0), "tools": tools, "result": ""}
        err = proc.stderr.read()
        if not result:
            return {"status": "error", "error": "claude -p exit %d: %s" % (proc.returncode, (err or "no result")[-300:]),
                    "cost": 0.0, "tools": tools, "result": ""}
        if result.get("is_error") or proc.returncode != 0:
            return {"status": "error", "error": "claude -p: %s" % str(result.get("result") or result.get("subtype"))[-300:],
                    "cost": float(result.get("total_cost_usd") or 0), "tools": tools, "result": ""}
        return {"status": "done", "error": "", "cost": float(result.get("total_cost_usd") or 0), "tools": tools,
                "result": str(result.get("result") or "")[-2000:]}

    def spend(self, vault, sid):
        """The close line, from the vault's cwd (spend scans the cwd's transcript dir). Best effort."""
        r = subprocess.run([sys.executable, str(HERE / "spend_readout.py"), "--session", sid, "--line"],
                           capture_output=True, text=True, cwd=str(vault))
        return r.stdout.strip() if r.returncode == 0 else ""


class FakeRunner:
    """A deterministic night for tests and --dry-run neighbours: two drafts, one report, no model."""

    def __init__(self):
        self.calls = []
        self.fail = None

    def __call__(self, p, vault, sid, prompt, on_event=None, scratch=None):
        self.calls.append({"project": p["id"], "cwd": str(vault), "session": sid, "prompt": prompt,
                           "scratch": str(scratch) if scratch else None,
                           "scratch_existed": bool(scratch) and Path(scratch).is_dir()})
        if self.fail:
            return {"status": "error", "error": self.fail, "cost": 0.0, "tools": [], "result": ""}
        v = Path(vault)
        tools = [
            {"name": "Read", "target": str(v / p["run_prompt"])},
            {"name": "Write", "target": str(v / "00_Inbox" / "00 - Research draft — A (2026-10-29).md")},
            {"name": "Write", "target": str(v / "00_Inbox" / "01 - Depth draft — B (2026-10-29).md")},
            {"name": "Write", "target": str(v / "Action Queue" / ("99 - Morning report — overnight, night 4: %s (2026-10-29).md" % p["name"]))},
        ]
        for t in tools:
            if on_event:
                on_event(dict(event="tool", **t))
        return {"status": "done", "error": "", "cost": 12.5, "tools": tools, "result": "Night done; report committed and pushed."}


# ── a night ─────────────────────────────────────────────────────────────────

def _rel(vault, target):
    try:
        return str(Path(target).resolve().relative_to(Path(vault).resolve()))
    except (ValueError, OSError):
        return None


def _tagged_drafts(vault, pid, date):
    """Inbox notes whose frontmatter marks them as this project's overnight draft for `date` — catches drafts
    a subagent wrote in scratch and the night copied in by Bash, which no Write/Edit target shows."""
    found = []
    for f in sorted((Path(vault) / "00_Inbox").glob("*.md")):
        try:
            head = f.read_text(encoding="utf-8", errors="replace").split("\n---", 1)[0]
        except OSError:
            continue
        tags = re.search(r"^tags:\s*\[([^\]]*)\]", head, re.M)
        status = re.search(r"^status:.*$", head, re.M)
        names = {t.strip().strip("'\"").lower() for t in tags.group(1).split(",")} if tags else set()
        if {"overnight-draft", pid} <= names and status and ("overnight " + date) in status.group(0):
            found.append("00_Inbox/" + f.name)
    return found


def _written(vault, tools, pid=None, date=None):
    drafts, report = [], ""
    for t in tools:
        if t.get("name") not in ("Write", "Edit"):
            continue
        rel = _rel(vault, t.get("target") or "")
        if not rel:
            continue
        if rel.startswith("00_Inbox/") and rel.endswith(".md") and rel not in drafts:
            drafts.append(rel)
        elif rel.startswith("Action Queue/") and rel.endswith(".md"):
            report = rel
    if pid and date:
        drafts += [d for d in _tagged_drafts(vault, pid, date) if d not in drafts]
    return drafts, report


def run(vault, pid, runner, store, now, on_event=None, dry_run=False):
    vault = Path(vault)
    p = plan(vault, pid)
    date = kst_date(now)
    prior = store.night(pid, date)
    if prior and prior.get("status") == "done":
        raise OvernightError("tonight's %s night (%s) is done: %s — one night per project per day"
                             % (pid, date, prior.get("report") or "no report recorded"))
    sid = str(uuid.uuid4())
    scratch = store.scratch(pid, date)
    prompt = user_prompt(p, sid, now, scratch)
    commits = commits_since(vault, p["folders"], kst_midnight(now).isoformat())
    row = {"v": 1, "project": pid, "date": date, "session": sid, "host": socket.gethostname(), "model": MODEL,
           "run_prompt": p["run_prompt"], "started": now.isoformat(), "ended": None, "status": "running",
           "commits_today": commits, "cost": 0.0, "tools": 0, "drafts": [], "report": "", "error": "",
           "spend_line": "", "result": "", "scratch": str(scratch)}
    if dry_run:
        row.update(status="dry-run", command=Runner().command(p, vault, sid, scratch), prompt=prompt)
        if on_event:
            on_event({"event": "dry-run", "project": pid, "date": date, "session": sid, "commits_today": commits,
                      "command": row["command"], "prompt": prompt})
        return row
    if on_event:
        on_event({"event": "started", "project": pid, "date": date, "session": sid, "commits_today": commits,
                  "run_prompt": p["run_prompt"]})
    store.save(row)
    scratch.mkdir(parents=True, exist_ok=True)
    r = runner(p, vault, sid, prompt, on_event=on_event, scratch=scratch)
    ended = datetime.now(timezone.utc).replace(microsecond=0) if os.environ.get("QUORUM_OVERNIGHT_NOW") is None else now
    drafts, report = _written(vault, r.get("tools") or [], pid, date)
    row.update(ended=ended.isoformat(), status=r["status"], cost=float(r.get("cost") or 0), tools=len(r.get("tools") or []),
               drafts=drafts, report=report, error=r.get("error") or "", result=r.get("result") or "")
    if r["status"] == "done" and hasattr(runner, "spend"):
        row["spend_line"] = runner.spend(vault, sid)
    if r["status"] == "done":
        shutil.rmtree(scratch, ignore_errors=True)
    store.save(row)
    if r["status"] != "done":
        if on_event:
            on_event({"event": "error", "project": pid, "date": date, "status": r["status"], "message": row["error"]})
        raise OvernightError("%s night %s %s: %s" % (pid, date, r["status"], row["error"]))
    if on_event:
        on_event({"event": "done", "project": pid, "date": date, "session": sid, "cost": row["cost"],
                  "drafts": len(drafts), "report": report, "tools": row["tools"]})
    return row


# ── the score ───────────────────────────────────────────────────────────────

def _blank():
    return {"drafted": 0, "kept": 0, "discarded": 0, "pending": 0}


def score(vault, ledger_rel, pid=None):
    """kept ÷ drafted per project and lane, from the vault's ledger note: a markdown table
    `| Night | Project | Draft | Lane | Verdict | Landed |` the morning session appends to ($0)."""
    path = Path(vault) / ledger_rel
    if not ledger_rel or not path.is_file():
        raise OvernightError("no overnight ledger at %s" % (path if ledger_rel else "(unset: overnight.ledger in the projects file)"))
    out = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 5 or cells[0].lower() == "night" or set(cells[0]) <= set("-: "):
            continue
        project = cells[1].lower()
        if not project or (pid and project != pid):
            continue
        lane = cells[3] or "?"
        verdict = cells[4].lower()
        key = "kept" if verdict.startswith("keep") else "discarded" if verdict.startswith("discard") else "pending"
        s = out.setdefault(project, dict(_blank(), by_lane={}))
        l = s["by_lane"].setdefault(lane, _blank())
        for d in (s, l):
            d["drafted"] += 1
            d[key] += 1
    return out


# ── CLI ─────────────────────────────────────────────────────────────────────

def _now():
    v = os.environ.get("QUORUM_OVERNIGHT_NOW")
    if v:
        return datetime.fromisoformat(v.replace("Z", "+00:00"))
    return datetime.now(timezone.utc).replace(microsecond=0)


def _usage():
    return __doc__.split("Usage:", 1)[1].rstrip()


def main(argv):
    if argv and argv[0] in ("-h", "--help", "help"):
        print("Usage:" + _usage(), file=sys.stderr)
        return 0
    if not argv:
        print("ERROR: which project? quorum overnight <project> [--dry-run] [--events] | list | score [project]\n\nUsage:"
              + _usage(), file=sys.stderr)
        return 2
    sub, rest = argv[0], argv[1:]
    flags = {a for a in rest if a.startswith("--")}
    pos = [a for a in rest if not a.startswith("--")]
    vault = default_vault()
    store = Store()
    events = "--events" in flags

    def emit(ev):
        if events:
            print(json.dumps(ev, ensure_ascii=False), flush=True)
        elif ev.get("event") == "tool":
            print("  · %s %s" % (ev["name"], ev["target"]), file=sys.stderr, flush=True)
        elif ev.get("event") == "started":
            print("overnight %s %s · session %s · %d commit%s under its folders today on this host" % (
                ev["project"], ev["date"], ev["session"], ev["commits_today"], "" if ev["commits_today"] == 1 else "s"),
                file=sys.stderr, flush=True)
        elif ev.get("event") == "dry-run":
            print("dry run — would start:\n  %s\n--- prompt ---\n%s\n--- commits under its folders today: %d"
                  % (" ".join(ev["command"]), ev["prompt"], ev["commits_today"]))

    try:
        if sub == "list":
            rows = store.nights(pos[0] if pos else None)
            if not rows:
                print("no nights on this host (%s)" % store.root)
                return 0
            for r in rows:
                print("%s  %-11s %-9s $%6.2f  %d draft%s  %s" % (
                    r["date"], r["project"], r["status"], r.get("cost") or 0, len(r.get("drafts") or []),
                    "" if len(r.get("drafts") or []) == 1 else "s", r.get("report") or r.get("error") or ""))
            return 0
        if sub == "score":
            data = radio_projects.read_file(vault)
            ledger = (data.get("overnight") or {}).get("ledger") or ""
            s = score(vault, ledger, pos[0] if pos else None)
            if not s:
                print("no ledger rows%s in %s" % ((" for %s" % pos[0]) if pos else "", ledger))
                return 0
            for project, v in sorted(s.items()):
                lanes = " · ".join("%s %d/%d" % (k, l["kept"], l["drafted"]) for k, l in sorted(v["by_lane"].items()))
                print("%-11s kept %d/%d (discarded %d, pending %d) · %s" % (
                    project, v["kept"], v["drafted"], v["discarded"], v["pending"], lanes))
            return 0
        pid = sub
        fake = "--fake" in flags or os.environ.get("QUORUM_OVERNIGHT_FAKE") == "1"
        runner = FakeRunner() if fake else Runner()
        row = run(vault, pid, runner, store, _now(), on_event=emit, dry_run="--dry-run" in flags)
        if row["status"] == "done" and not events:
            print("done %s %s · $%.2f · %d tool call%s · %d draft%s · %s\n%s" % (
                row["project"], row["date"], row["cost"], row["tools"], "" if row["tools"] == 1 else "s",
                len(row["drafts"]), "" if len(row["drafts"]) == 1 else "s", row["report"] or "no report recorded",
                row.get("spend_line") or ""))
        return 0
    except (OvernightError, radio_projects.ProjectsError) as e:
        if events:
            print(json.dumps({"event": "error", "message": str(e)}), flush=True)
        print("ERROR: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
