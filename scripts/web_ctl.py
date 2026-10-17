#!/usr/bin/env python3
"""`quorum web` — start and stop Radio Studio (QR-23). Python stdlib.

  quorum web start [--fake] [--port N] [--no-open]   build if stale, start detached, open the browser
  quorum web stop                                     stop it
  quorum web status                                   running? (exit 0) / stopped (exit 3)
  quorum web logs [-f]                                the server's log
  quorum web dev [--fake]                             Vite with hot reload + the API, foreground; Ctrl-C stops both
  quorum web build                                    npm ci (when the lockfile changed) + the production build

State (pid, log) lives in <repo>/.quorum/web/. The server binds 127.0.0.1 only; it is
owner-started, never a login item, never restarted on a crash.
"""
import json
import os
import shutil
import signal
import subprocess
import sys
import time
import urllib.request
import webbrowser
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
SERVER = HERE / "radio_web.py"
BUILD_INPUTS = ("src", "index.html", "vite.config.ts", "tsconfig.json", "package.json", "package-lock.json",
                "node_modules/.package-lock.json")


def _env_path(key, default):
    v = os.environ.get(key)
    return Path(v) if v else default


def web_dir():
    return _env_path("QUORUM_WEB_DIR", REPO / "radio-web")


def state_dir():
    return _env_path("QUORUM_WEB_STATE", REPO / ".quorum" / "web")


def port():
    return int(os.environ.get("QUORUM_WEB_PORT") or 4747)


def _newest(paths):
    t = 0.0
    for p in paths:
        if p.is_dir():
            for f in p.rglob("*"):
                if f.is_file():
                    t = max(t, f.stat().st_mtime)
        elif p.exists():
            t = max(t, p.stat().st_mtime)
    return t


def needs_deps(web):
    marker = web / "node_modules" / ".package-lock.json"
    return not marker.exists() or (web / "package-lock.json").stat().st_mtime > marker.stat().st_mtime


def needs_build(web):
    out = web / "dist" / "index.html"
    if not out.exists() or needs_deps(web):
        return True
    return _newest([web / x for x in BUILD_INPUTS]) > out.stat().st_mtime


def _alive(pid):
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def _pid():
    """The server's pid, or None; a stale pid file is removed."""
    f = state_dir() / "server.pid"
    if not f.exists():
        return None
    try:
        pid = int(f.read_text().strip())
    except ValueError:
        pid = 0
    if pid and _alive(pid):
        return pid
    f.unlink()
    return None


def _health(p, timeout=1.0):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d/api/health" % p, timeout=timeout) as r:
            return json.loads(r.read())
    except (OSError, ValueError):
        return None


def _url(p):
    return "http://127.0.0.1:%d" % p


def _npm(web, *args):
    if not shutil.which("npm"):
        raise SystemExit("ERROR: npm not found: Radio Studio's app needs Node (nvm install --lts)")
    print("  npm %s (radio-web)…" % " ".join(args), flush=True)
    p = subprocess.run(["npm"] + list(args), cwd=str(web))
    if p.returncode != 0:
        raise SystemExit("ERROR: npm %s failed in %s" % (" ".join(args), web))


def build(force=False):
    web = web_dir()
    if needs_deps(web):
        _npm(web, "ci", "--no-audit", "--no-fund")
    if force or needs_build(web):
        _npm(web, "run", "build")
        return True
    return False


def _open(url):
    if os.environ.get("QUORUM_WEB_NO_OPEN") == "1":
        return
    for cmd in ("wslview",):  # WSL: the Windows browser
        if shutil.which(cmd):
            subprocess.run([cmd, url], capture_output=True)
            return
    webbrowser.open(url)


def start(args):
    p = port()
    fake = "--fake" in args
    no_open = "--no-open" in args
    if "--port" in args:
        p = int(args[args.index("--port") + 1])
        os.environ["QUORUM_WEB_PORT"] = str(p)
    pid = _pid()
    if pid and _health(p):
        print("Radio Studio is already running: %s (pid %d)" % (_url(p), pid))
        if not no_open:
            _open(_url(p))
        return 0
    if _health(p):
        print("ERROR: port %d answers but is not ours (no pid file); stop that server or use --port" % p, file=sys.stderr)
        return 1
    try:
        if build():
            print("  built radio-web/dist")
    except SystemExit as e:
        print(e, file=sys.stderr)
        return 1
    sd = state_dir()
    sd.mkdir(parents=True, exist_ok=True)
    log = open(sd / "server.log", "a")
    log.write("\n--- start %s%s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), " (fake)" if fake else ""))
    log.flush()
    env = dict(os.environ, QUORUM_WEB_PORT=str(p))
    proc = subprocess.Popen([sys.executable, str(SERVER), "--port", str(p)] + (["--fake"] if fake else []),
                            stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env,
                            start_new_session=True, close_fds=True)
    (sd / "server.pid").write_text(str(proc.pid))
    for _ in range(100):
        if _health(p, 0.3):
            break
        if proc.poll() is not None:
            (sd / "server.pid").unlink(missing_ok=True)
            print("ERROR: the server exited (code %s); see quorum web logs" % proc.returncode, file=sys.stderr)
            return 1
        time.sleep(0.1)
    else:
        print("ERROR: the server did not answer within 10 s; see quorum web logs", file=sys.stderr)
        return 1
    print("Radio Studio: %s (pid %d%s) — stop with: quorum web stop" % (_url(p), proc.pid, ", fake model" if fake else ""))
    if not no_open:
        _open(_url(p))
    return 0


def stop(_args):
    pid = _pid()
    if not pid:
        print("Radio Studio is not running")
        return 0
    os.kill(pid, signal.SIGTERM)
    for _ in range(50):
        if not _alive(pid):
            break
        time.sleep(0.1)
    else:
        os.kill(pid, signal.SIGKILL)
    (state_dir() / "server.pid").unlink(missing_ok=True)
    print("Radio Studio stopped (pid %d)" % pid)
    return 0


def status(_args):
    pid, p = _pid(), port()
    h = _health(p) if pid else None
    if not (pid and h):
        print("Radio Studio: stopped%s" % (" (pid %d does not answer)" % pid if pid else ""))
        return 3
    runs = h.get("running") or []
    print("Radio Studio: running at %s (pid %d)%s%s%s" % (
        _url(p), pid, ", fake model" if h.get("fake") else "", "" if h.get("voice") else ", no voice",
        (", generating: %s %s" % (runs[0]["kind"], runs[0]["project"])) if runs else ""))
    print("  vault %s · store %s" % (h.get("vault"), h.get("store")))
    if needs_build(web_dir()):
        print("  the app's sources changed since the build: quorum web stop && quorum web start rebuilds it")
    return 0


def logs(args):
    f = state_dir() / "server.log"
    if not f.exists():
        print("no log yet (%s)" % f)
        return 0
    if "-f" in args:
        os.execvp("tail", ["tail", "-n", "40", "-f", str(f)])
    print("".join(f.read_text(errors="replace").splitlines(True)[-60:]), end="")
    return 0


def dev(args):
    """Vite (5173, hot reload) in front of the API (4747); both in the foreground."""
    web, p = web_dir(), port()
    if needs_deps(web):
        _npm(web, "ci", "--no-audit", "--no-fund")
    api = None
    if not _health(p):
        api = subprocess.Popen([sys.executable, str(SERVER), "--port", str(p)] + (["--fake"] if "--fake" in args else []))
    else:
        print("  the API is already running on %d (quorum web start); Vite proxies to it" % p)
    print("Radio Studio (dev): http://127.0.0.1:5173 — Ctrl-C stops it")
    try:
        return subprocess.call(["npm", "run", "dev"], cwd=str(web), env=dict(os.environ, QUORUM_WEB_PORT=str(p)))
    except KeyboardInterrupt:
        return 0
    finally:
        if api:
            api.terminate()
            api.wait(timeout=5)


USAGE = __doc__.split("\n\n")[1]


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(USAGE, file=sys.stderr)
        return 2 if not argv else 0
    cmd, rest = argv[0], argv[1:]
    fn = {"start": start, "stop": stop, "status": status, "logs": logs, "dev": dev,
          "build": lambda a: (build(force=True), 0)[1]}.get(cmd)
    if not fn:
        print("ERROR: unknown web command %r\n\n%s" % (cmd, USAGE), file=sys.stderr)
        return 2
    return fn(rest)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
