#!/usr/bin/env python3
"""Tests for web_ctl.py — `quorum web start|stop|status` (QR-23): staleness of the
build and the deps, and a real start → status → stop of the server (fake model)."""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import web_ctl  # noqa: E402
from radio_test import Fixture  # noqa: E402


def touch(p, t):
    p.parent.mkdir(parents=True, exist_ok=True)
    if not p.exists():
        p.write_text("x")
    os.utime(p, (t, t))


class StaleCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.web = Path(self.tmp.name)
        touch(self.web / "package-lock.json", 100)
        touch(self.web / "node_modules" / ".package-lock.json", 200)
        touch(self.web / "src" / "App.tsx", 300)
        touch(self.web / "index.html", 300)
        touch(self.web / "dist" / "index.html", 400)

    def tearDown(self):
        self.tmp.cleanup()

    def test_fresh(self):
        self.assertFalse(web_ctl.needs_deps(self.web))
        self.assertFalse(web_ctl.needs_build(self.web))

    def test_a_source_newer_than_the_build_rebuilds(self):
        touch(self.web / "src" / "views" / "New.tsx", 500)
        self.assertTrue(web_ctl.needs_build(self.web))

    def test_no_build_builds(self):
        (self.web / "dist" / "index.html").unlink()
        self.assertTrue(web_ctl.needs_build(self.web))

    def test_a_lockfile_newer_than_the_install_reinstalls(self):
        touch(self.web / "package-lock.json", 250)
        self.assertTrue(web_ctl.needs_deps(self.web))
        self.assertTrue(web_ctl.needs_build(self.web))

    def test_no_node_modules_installs(self):
        (self.web / "node_modules" / ".package-lock.json").unlink()
        self.assertTrue(web_ctl.needs_deps(self.web))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class LifecycleCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.fx = Fixture(root)
        self.state = root / "state"
        self.web = root / "web"
        for f in ("package-lock.json", "node_modules/.package-lock.json", "src/App.tsx", "dist/index.html"):
            touch(self.web / f, 100 + len(f))
        touch(self.web / "dist" / "index.html", 1000)
        self.port = free_port()
        self.env = dict(os.environ, **self.fx.env(), QUORUM_WEB_STATE=str(self.state), QUORUM_WEB_DIR=str(self.web),
                        QUORUM_WEB_PORT=str(self.port), QUORUM_WEB_NO_OPEN="1")

    def tearDown(self):
        self.ctl("stop")
        self.tmp.cleanup()

    def ctl(self, *args):
        return subprocess.run([sys.executable, str(HERE / "quorum_cli.py"), "web"] + list(args), env=self.env,
                              capture_output=True, text=True, timeout=60)

    def test_start_status_stop(self):
        p = self.ctl("start", "--fake")
        self.assertEqual(p.returncode, 0, p.stderr + p.stdout)
        self.assertIn("http://127.0.0.1:%d" % self.port, p.stdout)
        pid = int((self.state / "server.pid").read_text())
        st = self.ctl("status")
        self.assertEqual(st.returncode, 0, st.stdout)
        self.assertIn("running", st.stdout)
        again = self.ctl("start")
        self.assertEqual(again.returncode, 0)
        self.assertIn("already running", again.stdout)
        self.assertEqual(int((self.state / "server.pid").read_text()), pid)
        self.assertTrue((self.state / "server.log").exists())
        s = self.ctl("stop")
        self.assertEqual(s.returncode, 0, s.stderr)
        self.assertFalse((self.state / "server.pid").exists())
        for _ in range(30):
            try:
                os.kill(pid, 0)
            except OSError:
                break
            time.sleep(0.1)
        else:
            self.fail("the server is still running")
        st = self.ctl("status")
        self.assertEqual(st.returncode, 3)
        self.assertIn("stopped", st.stdout)

    def test_stop_when_stopped_is_fine(self):
        p = self.ctl("stop")
        self.assertEqual(p.returncode, 0)
        self.assertIn("not running", p.stdout)

    def test_a_stale_pid_file_is_cleared(self):
        self.state.mkdir(parents=True)
        (self.state / "server.pid").write_text("99999999")
        st = self.ctl("status")
        self.assertEqual(st.returncode, 3)
        self.assertFalse((self.state / "server.pid").exists())

    def test_unknown_subcommand(self):
        self.assertEqual(self.ctl("dance").returncode, 2)


if __name__ == "__main__":
    unittest.main()
