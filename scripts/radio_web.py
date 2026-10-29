#!/usr/bin/env python3
"""Radio Studio's API (QR-22): Python stdlib, 127.0.0.1 only, one port for the API
and the built React app (radio-web/dist).

  python3 scripts/radio_web.py [--port 4747] [--fake]

Episodes come from the per-host store (radio_store), projects from the vault
(radio_projects), the cast from radio/cast.json. A generation runs `radio.py …
--events` as a child process and its JSON lines are relayed over SSE. Started and
stopped by `quorum web start|stop` (scripts/web_ctl.py); never a login item.
"""
import argparse
import json
import mimetypes
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import uuid
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
import radio  # noqa: E402
import radio_projects  # noqa: E402
import radio_room  # noqa: E402
import radio_store  # noqa: E402

DIST = REPO / "radio-web" / "dist"
DEFAULT_PORT = 4747
DEV_PORT = 5173
RANGES = ("today", "yesterday", "week")
RANGE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}(\.\.\d{4}-\d{2}-\d{2})?$")
JOB_KEEP = 50


class HttpError(Exception):
    def __init__(self, code, msg, **extra):
        super().__init__(msg)
        self.code, self.extra = code, extra


def voice_ready():
    """The sidecar, its pinned weights' package and ffmpeg: what an MP3 needs."""
    return bool(shutil.which("node") and shutil.which("ffmpeg")
                and (REPO / "radio-voice" / "node_modules" / "kokoro-js").is_dir())


class Job:
    def __init__(self, kind, project, argv, env):
        self.id = uuid.uuid4().hex[:12]
        self.kind, self.project = kind, project
        self.events, self.cond = [], threading.Condition()
        self.episode, self.code, self.stderr = None, None, []
        self.started = time.time()
        self.proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env,
                                     start_new_session=True)
        threading.Thread(target=self._read_err, daemon=True).start()
        threading.Thread(target=self._read_out, daemon=True).start()

    def push(self, ev):
        with self.cond:
            self.events.append(ev)
            self.cond.notify_all()

    def _read_err(self):
        for line in self.proc.stderr:
            self.stderr = (self.stderr + [line.rstrip()])[-40:]
        self.proc.stderr.close()

    def _read_out(self):
        for line in self.proc.stdout:
            try:
                ev = json.loads(line)
            except ValueError:
                continue
            if ev.get("event") == "started":
                self.episode = ev.get("id")
            self.push(ev)
        self.proc.stdout.close()
        self.code = self.proc.wait()
        if self.code != 0 and not any(e.get("event") == "error" for e in self.events):
            self.push({"event": "error", "message": "\n".join(self.stderr[-6:]) or "exit %d" % self.code})
        self.push({"event": "exit", "code": self.code})

    @property
    def done(self):
        return self.code is not None and self.events and self.events[-1].get("event") == "exit"

    def cancel(self):
        if self.code is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass

    def view(self):
        return {"id": self.id, "kind": self.kind, "project": self.project, "episode": self.episode,
                "done": bool(self.done), "code": self.code, "events": len(self.events),
                "started": datetime.fromtimestamp(self.started, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")}


class App:
    def __init__(self, vault, store_root=None, roots=None, fake=False, voice=None, env=None, dist=DIST,
                 cast_path=radio.CAST_FILE, lock_path=radio.LOCK_FILE, commit_cast=True, roles_dir=None):
        self.vault = Path(vault)
        self.env = dict(os.environ, **(env or {}))
        # the sweep runs on the same clock the episodes are made with
        self.store = radio_store.Store(store_root, now=self.env.get("QUORUM_RADIO_NOW"))
        self.roots = roots or radio._roots()
        self.fake = fake
        self.voice = voice_ready() if voice is None else voice
        self.env.setdefault("QUORUM_RADIO_STORE", str(self.store.root))
        self.env.setdefault("QUORUM_VAULT", str(self.vault))
        self.dist = Path(dist)
        self.cast_path, self.lock_path, self.commit_cast = Path(cast_path), Path(lock_path), commit_cast
        self.roles_dir = Path(roles_dir or radio.ROLES_DIR)
        self.jobs, self.lock = {}, threading.Lock()

    # ── projects ────────────────────────────────────────────────────────────
    def projects(self):
        return [dict(cfg, id=pid) for pid, cfg in radio_projects.load(self.vault).items()]

    # ── cast ────────────────────────────────────────────────────────────────
    def cast(self):
        cast = radio.load_cast(self.cast_path, self.lock_path)
        return {"cast": [{"id": d["id"], "name": d["name"], "voice": d["voice"], "pan": d["pan"], "recap": d["recap"],
                          "question": d["reads"].get("question") or None,
                          "angle": (d.get("brainstorm") or {}).get("angle"),
                          "default": d["id"] in ("builder", "verifier", "architect", "product")}
                         for d in cast],
                "free_voices": radio.free_voices(self.cast_path, self.lock_path), "pans": list(radio.PANS)}

    def add_specialty(self, spec):
        try:
            radio.add_specialty(spec, self.cast_path, self.lock_path, roles_dir=self.roles_dir)
        except radio.CastError as e:
            raise HttpError(400, str(e), errors=[str(e)])
        if self.commit_cast:
            rels = [str(p.relative_to(REPO)) for p in (self.cast_path, self.lock_path,
                                                         self.roles_dir / ("%s.md" % spec.get("id")))]
            subprocess.run(["git", "-C", str(REPO), "add", "--"] + rels, capture_output=True)
            subprocess.run(["git", "-C", str(REPO), "commit", "-q", "-m",
                            "radio: add specialty %s (%s, QR-27)" % (spec.get("id"), spec.get("voice")), "--"] + rels,
                           capture_output=True)
        return self.cast()

    # ── the tuning loop: feedback, roles, replay ────────────────────────────
    def _speaker_of(self, eid, target, n):
        sc = self.store.script(eid) or {}
        rows = (sc.get("room") or {}).get("turns", []) if target == "turn" else sc.get("lines", [])
        row = next((x for x in rows if x.get("n", x.get("i")) == n), None)
        if not row or row["speaker"] == radio.HOST and target == "turn" and row.get("kind") != "close":
            return None, None
        return row["speaker"], row["text"]

    def add_feedback(self, eid, body):
        try:
            n = int(body.get("n"))
        except (TypeError, ValueError):
            raise HttpError(400, "n must be a turn or line number")
        speaker, _ = self._speaker_of(eid, body.get("target"), n)
        if not speaker:
            raise HttpError(400, "no such %s %s in this episode" % (body.get("target"), body.get("n")))
        try:
            return self.store.add_feedback(eid, dict(body, n=n, speaker=speaker))
        except ValueError as e:
            raise HttpError(400, str(e))

    def roles(self):
        counts = {}
        for m in self.store.list():
            for f in self.store.feedback(m["id"]):
                c = counts.setdefault(f["speaker"], {"up": 0, "down": 0})
                c[f["vote"]] += 1
        return [{"id": d["id"], "name": d["name"], "voice": d["voice"], "angle": (d.get("brainstorm") or {}).get("angle"),
                 "question": d["reads"].get("question") or None, "has_role": (self.roles_dir / ("%s.md" % d["id"])).exists(),
                 **counts.get(d["id"], {"up": 0, "down": 0})}
                for d in radio.load_cast(self.cast_path, self.lock_path)]

    def role(self, sid):
        if sid not in {d["id"] for d in radio.load_cast(self.cast_path, self.lock_path)}:
            raise HttpError(404, "no specialty %s" % sid)
        p = self.roles_dir / ("%s.md" % sid)
        fb = []
        for m in self.store.list():
            for f in self.store.feedback(m["id"]):
                if f["speaker"] == sid:
                    _, text = self._speaker_of(m["id"], f["target"], f["n"])
                    turn = f["n"] if f["target"] == "turn" else None
                    if f["target"] == "line":   # a broadcast line replays through the room turn it came from
                        line = next((l for l in (self.store.script(m["id"]) or {}).get("lines", []) if l["i"] == f["n"]), {})
                        mm = re.match(r"room turn (\d+)$", ((line.get("source") or {}).get("ref") or ""))
                        turn = int(mm.group(1)) if mm else None
                    fb.append(dict(f, episode=m["id"], title=m.get("title"), kind=m["kind"], excerpt=(text or "")[:600],
                                   replay_turn=turn))
        fb.sort(key=lambda x: x["at"], reverse=True)
        return {"id": sid, "text": p.read_text() if p.exists() else "", "path": str(p), "feedback": fb}

    def save_role(self, sid, text):
        self.role(sid)
        text = (text or "").strip()
        if len(text) < 20:
            raise HttpError(400, "a role needs some text")
        p = self.roles_dir / ("%s.md" % sid)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text + "\n")
        committed = None
        if self.commit_cast:
            rel = str(p.relative_to(REPO))
            subprocess.run(["git", "-C", str(REPO), "add", "--", rel], capture_output=True)
            c = subprocess.run(["git", "-C", str(REPO), "commit", "-q", "-m", "radio: tune the %s role" % sid, "--", rel],
                               capture_output=True, text=True)
            if c.returncode == 0:
                committed = subprocess.run(["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"],
                                           capture_output=True, text=True).stdout.strip()
        return dict(self.role(sid), committed=committed)

    def project_dirs(self, pid):
        """A project's readable dirs: its repos (by path) and its vault folders."""
        cfg = radio_projects.load(self.vault).get(pid)
        if not cfg:
            return []
        repos = [str(Path(r) / n) for n in cfg.get("repos", []) for r in self.roots if (Path(r) / n / ".git").exists()]
        return repos + [str(self.vault / f) for f in cfg.get("folders", [])]

    def replay(self, sid, body):
        eid = body.get("episode")
        try:
            sc = self.store.script(eid)
        except KeyError:
            raise HttpError(404, "no episode %s" % eid)
        room = (sc or {}).get("room")
        if not room:
            raise HttpError(400, "only a brainstorm's room turns replay")
        try:
            n = int(body.get("turn"))
        except (TypeError, ValueError):
            raise HttpError(400, "turn must be a number")
        if not (0 <= n < len(room["turns"])) or room["turns"][n]["speaker"] != sid or room["turns"][n]["kind"] != "answer":
            raise HttpError(400, "turn %s is not %s's answer" % (n, sid))
        dirs = room.get("dirs") or self.project_dirs(room.get("project"))   # rooms made before 10-17 19:00 kept none
        model = radio_room.FakeRoomModel() if self.fake else radio_room.RoomModel(
            Path(os.path.expanduser("~/.cache/quorum/radio/session")), dirs)
        cast = radio.load_cast(self.cast_path, self.lock_path)
        text = body.get("text")
        if text is None:
            text = radio.role_text(sid, "room", self.roles_dir)
        try:
            r = radio_room.replay_turn(room, cast, n, radio.role_text_from(text, "room"), model)
        except radio.ModelError as e:
            raise HttpError(502, "replay failed: %s" % e)
        return dict(r, cost=getattr(model, "cost", 0.0))

    # ── generation ──────────────────────────────────────────────────────────
    def generate(self, body):
        kind, pid = body.get("kind"), body.get("project")
        projects = radio_projects.load(self.vault)
        if kind not in radio_store.KINDS:
            raise HttpError(400, "kind must be recap or brainstorm")
        if pid not in projects:
            raise HttpError(400, "unknown project %r" % pid)
        if not projects[pid]["active"]:
            raise HttpError(400, "%s is inactive" % pid)
        argv = [sys.executable, str(HERE / "radio.py")]
        if kind == "recap":
            rng = str(body.get("range") or "today")
            if rng not in RANGES and not RANGE_RE.match(rng):
                raise HttpError(400, "range: today | yesterday | week | YYYY-MM-DD | A..B")
            invite = [x for x in (body.get("with") or []) if isinstance(x, str)]
            if body.get("with_operator") and "operator" not in invite:
                invite.append("operator")
            if any(x not in radio.RECAP_OPT_IN for x in invite):
                raise HttpError(400, "with: %s only" % ", ".join(radio.RECAP_OPT_IN))
            argv += [pid, rng] + (["--with", ",".join(invite)] if invite else [])
        else:
            q = re.sub(r"\s+", " ", str(body.get("question") or "")).strip()
            if not q:
                raise HttpError(400, "a brainstorm takes one question")
            argv += ["brainstorm", pid, q]
            if body.get("specialties"):
                argv += ["--with", ",".join(str(x) for x in body["specialties"])]
            if body.get("context"):
                argv += ["--context", ",".join(str(x) for x in body["context"])]
        argv += ["--events"] + (["--fake"] if self.fake else []) + ([] if self.voice else ["--no-voice"])
        with self.lock:
            if any(not j.done for j in self.jobs.values()):
                raise HttpError(409, "a generation is already running; wait for it or cancel it")
            job = Job(kind, pid, argv, self.env)
            self.jobs[job.id] = job
            for old in sorted(self.jobs.values(), key=lambda j: j.started)[:-JOB_KEEP]:
                self.jobs.pop(old.id, None)
        return job

    def job(self, jid):
        j = self.jobs.get(jid)
        if not j:
            raise HttpError(404, "no job %s" % jid)
        return j


def _allowed_hosts(port):
    return {"127.0.0.1:%d" % port, "localhost:%d" % port, "127.0.0.1", "localhost"}


class Handler(BaseHTTPRequestHandler):
    server_version = "QuorumRadio/1"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        if os.environ.get("QUORUM_WEB_QUIET") != "1":
            sys.stderr.write("%s %s\n" % (datetime.now().strftime("%H:%M:%S"), fmt % args))

    @property
    def app(self):
        return self.server.app

    # ── plumbing ────────────────────────────────────────────────────────────
    def _send(self, code, body=b"", ctype="application/json", headers=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, ensure_ascii=False).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store" if ctype.startswith("application/json") else "no-cache")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _json_body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if not n:
            return {}
        try:
            o = json.loads(self.rfile.read(n))
        except ValueError:
            raise HttpError(400, "the body is not JSON")
        if not isinstance(o, dict):
            raise HttpError(400, "the body must be a JSON object")
        return o

    def _guard(self):
        """127.0.0.1 only, and no other site may drive it: the Host must be ours (no DNS
        rebinding) and an Origin, when sent, must be this server or the Vite dev server."""
        port = self.server.server_address[1]
        if (self.headers.get("Host") or "") not in _allowed_hosts(port):
            raise HttpError(403, "foreign Host")
        origin = self.headers.get("Origin")
        if origin and origin not in {"http://%s" % h for h in _allowed_hosts(port) if ":" in h} | {
                "http://localhost:%d" % DEV_PORT, "http://127.0.0.1:%d" % DEV_PORT}:
            raise HttpError(403, "foreign Origin")

    def _dispatch(self):
        try:
            self._guard()
            u = urlparse(self.path)
            if u.path.startswith("/api/"):
                return self._api(u)
            if self.command not in ("GET", "HEAD"):
                raise HttpError(405, "method not allowed")
            return self._static(unquote(u.path))
        except HttpError as e:
            self._send(e.code, dict({"error": str(e)}, **e.extra))
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:  # noqa: BLE001 — the server keeps serving; the error goes to the client and the log
            self.log_message("ERROR %s: %r", self.path, e)
            try:
                self._send(500, {"error": "%s: %s" % (type(e).__name__, e)})
            except OSError:
                pass

    do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = do_HEAD = _dispatch

    # ── static ──────────────────────────────────────────────────────────────
    def _static(self, path):
        dist = self.app.dist.resolve()
        if not (dist / "index.html").exists():
            return self._send(503, "<!doctype html><meta charset=utf-8><title>Radio</title><p>The Radio web app is not "
                                   "built: run <code>quorum web start</code>.", "text/html; charset=utf-8")
        rel = path.lstrip("/")
        f = (dist / rel).resolve() if rel else dist / "index.html"
        if dist not in f.parents and f != dist / "index.html":
            raise HttpError(404, "not found")
        if not f.is_file():
            if rel.startswith("assets/") or "." in rel.rsplit("/", 1)[-1]:
                raise HttpError(404, "not found")
            f = dist / "index.html"   # the app's own routes
        ctype = mimetypes.guess_type(str(f))[0] or "application/octet-stream"
        if ctype.startswith("text/") or ctype in ("application/javascript",):
            ctype += "; charset=utf-8"
        cache = {"Cache-Control": "public, max-age=31536000, immutable"} if rel.startswith("assets/") else {}
        self._send(200, f.read_bytes(), ctype, cache)

    # ── the API ─────────────────────────────────────────────────────────────
    def _api(self, u):
        parts = [unquote(p) for p in u.path.split("/")[2:] if p]
        m, a = self.command, self.app
        q = parse_qs(u.query)
        if parts == ["health"] and m == "GET":
            return self._send(200, {"ok": True, "fake": a.fake, "voice": a.voice, "claude": bool(shutil.which("claude")),
                                    "vault": str(a.vault), "store": str(a.store.root),
                                    "running": [j.view() for j in a.jobs.values() if not j.done]})
        if parts[:1] == ["projects"]:
            return self._projects(parts[1:], m)
        if parts == ["cast"]:
            if m == "GET":
                return self._send(200, a.cast())
            if m == "POST":
                return self._send(200, a.add_specialty(self._json_body()))
        if parts[:1] == ["episodes"] and len(parts) >= 3 and parts[2] == "feedback":
            if len(parts) == 3 and m == "GET":
                try:
                    return self._send(200, {"feedback": a.store.feedback(parts[1])})
                except KeyError:
                    raise HttpError(404, "no episode %s" % parts[1])
            if len(parts) == 3 and m == "POST":
                try:
                    return self._send(200, a.add_feedback(parts[1], self._json_body()))
                except KeyError:
                    raise HttpError(404, "no episode %s" % parts[1])
            if len(parts) == 4 and m == "DELETE":
                try:
                    a.store.remove_feedback(parts[1], parts[3])
                except KeyError:
                    raise HttpError(404, "no episode %s" % parts[1])
                return self._send(200, {"deleted": parts[3]})
        if parts[:1] == ["episodes"]:
            return self._episodes(parts[1:], m, q)
        if parts == ["roles"] and m == "GET":
            return self._send(200, {"roles": a.roles()})
        if len(parts) == 2 and parts[0] == "roles":
            if m == "GET":
                return self._send(200, a.role(parts[1]))
            if m == "PUT":
                return self._send(200, a.save_role(parts[1], self._json_body().get("text")))
        if len(parts) == 3 and parts[0] == "roles" and parts[2] == "replay" and m == "POST":
            a.role(parts[1])
            return self._send(200, a.replay(parts[1], self._json_body()))
        if parts == ["generate"] and m == "POST":
            job = a.generate(self._json_body())
            return self._send(202, {"job": job.id})
        if parts == ["jobs"] and m == "GET":
            return self._send(200, {"jobs": [j.view() for j in sorted(a.jobs.values(), key=lambda j: -j.started)]})
        if len(parts) == 3 and parts[0] == "jobs" and parts[2] == "events" and m == "GET":
            return self._sse(a.job(parts[1]))
        if len(parts) == 3 and parts[0] == "jobs" and parts[2] == "cancel" and m == "POST":
            j = a.job(parts[1])
            j.cancel()
            for _ in range(50):
                if j.done:
                    break
                time.sleep(0.1)
            if j.episode:
                try:
                    a.store.delete(j.episode)
                except KeyError:
                    pass
            return self._send(200, j.view())
        raise HttpError(404, "no such API: %s %s" % (m, u.path))

    def _projects(self, rest, m):
        a = self.app
        try:
            if not rest and m == "GET":
                return self._send(200, {"projects": a.projects()})
            if rest == ["candidates"] and m == "GET":
                return self._send(200, {"repos": radio_projects.candidates(a.vault, a.roots),
                                        "folders": radio_projects.free_folders(a.vault)})
            if rest == ["derive"] and m == "POST":
                body = self._json_body()
                if not (body.get("repo") or body.get("folder")):
                    raise HttpError(400, "derive takes a repo or a folder")
                if body.get("folder") and not radio_projects._safe(body["folder"]):
                    raise HttpError(400, "not a vault folder")
                d = radio_projects.derive(a.vault, a.roots, repo=body.get("repo"), folder=body.get("folder"))
                d["check"] = radio_projects.check(d["spec"], a.vault, a.roots, existing=radio_projects.load(a.vault))
                return self._send(200, d)
            if rest == ["check"] and m == "POST":
                body = self._json_body()
                editing = body.pop("editing", None)   # an edit is never in conflict with itself
                others = {k: v for k, v in radio_projects.load(a.vault).items() if k != editing}
                return self._send(200, radio_projects.check(body, a.vault, a.roots, existing=others))
            if not rest and m == "POST":
                return self._send(200, radio_projects.add(a.vault, self._json_body(), a.roots))
            if len(rest) == 1 and m == "PATCH":
                body = self._json_body()
                if set(body) == {"active"}:
                    r = radio_projects.set_active(a.vault, rest[0], body["active"])
                else:
                    r = radio_projects.update(a.vault, rest[0], body, a.roots)
                return self._send(200, r)
        except radio_projects.ProjectsError as e:
            raise HttpError(400, str(e), errors=e.errors)
        except KeyError as e:
            raise HttpError(404, "no project %s" % e)
        raise HttpError(404, "no such API")

    def _episodes(self, rest, m, q):
        st = self.app.store
        try:
            if not rest and m == "GET":
                eps = st.list()
                for k in ("project", "kind"):
                    if q.get(k):
                        eps = [e for e in eps if e.get(k) == q[k][0]]
                return self._send(200, {"episodes": eps})
            if len(rest) == 1 and m == "GET":
                meta = next((e for e in st.list(sweep=False) if e["id"] == rest[0]), None) or st.meta(rest[0])
                return self._send(200, {"meta": meta, "script": st.script(rest[0])})
            if len(rest) == 1 and m == "DELETE":
                st.delete(rest[0])
                return self._send(200, {"deleted": rest[0]})
            if rest[1:] == ["keep"] and m == "POST":
                return self._send(200, st.keep(rest[0], bool(self._json_body().get("kept", True))))
            if rest[1:] == ["script.md"] and m == "GET":
                md = st.script_md(rest[0])
                if md is None:
                    raise HttpError(404, "no script yet")
                h = {"Content-Disposition": 'attachment; filename="%s.md"' % rest[0]} if q.get("download") else {}
                return self._send(200, md, "text/markdown; charset=utf-8", h)
            if rest[1:] == ["room.md"] and m == "GET":
                md = st.text(rest[0], "room.md")
                if md is None:
                    raise HttpError(404, "no room for this episode (a recap has none)")
                h = {"Content-Disposition": 'attachment; filename="%s-room.md"' % rest[0]} if q.get("download") else {}
                return self._send(200, md, "text/markdown; charset=utf-8", h)
            if rest[1:] == ["audio"] and m in ("GET", "HEAD"):
                return self._audio(st, rest[0])
        except KeyError:
            raise HttpError(404, "no episode %s" % rest[0])
        raise HttpError(404, "no such API")

    def _audio(self, st, eid):
        p = st.audio(eid)
        if not p:
            raise HttpError(404, "no audio for %s" % eid)
        size = p.stat().st_size
        rng = self.headers.get("Range")
        base = {"Accept-Ranges": "bytes"}
        if not rng:
            return self._send(200, p.read_bytes(), "audio/mpeg", base)
        mm = re.match(r"bytes=(\d*)-(\d*)$", rng.strip())
        if not mm or (not mm.group(1) and not mm.group(2)):
            raise HttpError(416, "bad range")
        if mm.group(1):
            start = int(mm.group(1))
            end = min(int(mm.group(2)), size - 1) if mm.group(2) else size - 1
        else:
            start, end = max(0, size - int(mm.group(2))), size - 1
        if start >= size or end < start:
            self.send_response(416)
            self.send_header("Content-Range", "bytes */%d" % size)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        with open(p, "rb") as f:
            f.seek(start)
            body = f.read(end - start + 1)
        self._send(206, body, "audio/mpeg", dict(base, **{"Content-Range": "bytes %d-%d/%d" % (start, end, size)}))

    def _sse(self, job):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        sent = 0
        while True:
            with job.cond:
                if sent >= len(job.events) and not job.done:
                    job.cond.wait(timeout=15)
                batch = job.events[sent:]
            try:
                if batch:
                    for ev in batch:
                        self.wfile.write(("data: %s\n\n" % json.dumps(ev, ensure_ascii=False)).encode())
                    sent += len(batch)
                else:
                    self.wfile.write(b": keep-alive\n\n")
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                return
            if job.done and sent >= len(job.events):
                return


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def make_server(app, host="127.0.0.1", port=DEFAULT_PORT):
    if host not in ("127.0.0.1", "localhost"):
        raise SystemExit("Radio's web binds 127.0.0.1 only (QR-22)")
    s = Server((host, port), Handler)
    s.app = app
    return s


def main(argv):
    ap = argparse.ArgumentParser(prog="radio_web", description="Radio Studio's API + the built web app, on 127.0.0.1")
    ap.add_argument("--port", type=int, default=int(os.environ.get("QUORUM_WEB_PORT") or DEFAULT_PORT))
    ap.add_argument("--fake", action="store_true", help="generate with the fake model and the fake voice (no calls)")
    o = ap.parse_args(argv)
    fake = o.fake or os.environ.get("QUORUM_RADIO_FAKE") == "1"
    app = App(vault=radio._vault(), fake=fake)
    srv = make_server(app, "127.0.0.1", o.port)
    print("Radio Studio on http://127.0.0.1:%d (vault %s, store %s%s)" % (
        o.port, app.vault, app.store.root, ", FAKE model" if fake else ""), flush=True)
    signal.signal(signal.SIGTERM, lambda *_: threading.Thread(target=srv.shutdown, daemon=True).start())
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        for j in app.jobs.values():
            j.cancel()
        srv.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
