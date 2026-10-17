#!/usr/bin/env python3
"""Tests for radio_web.py — the Radio Studio API (QR-22, QR-25): a real server on
an ephemeral port over a fixture vault and store, the radio run in its fake mode."""
import json
import shutil
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from datetime import timedelta
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import radio_web  # noqa: E402
from radio_test import CAST, LOCK, NOW, Fixture, git  # noqa: E402


class WebCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(cls.tmp.name)
        cls.fx = Fixture(root)
        git(cls.fx.vault, "add", "-A")
        git(cls.fx.vault, "commit", "-qm", "projects", "--allow-empty")
        cls.cast_p, cls.lock_p = root / "cast.json", root / "voices.lock"
        shutil.copy(CAST, cls.cast_p)
        shutil.copy(LOCK, cls.lock_p)
        dist = root / "dist"
        (dist / "assets").mkdir(parents=True)
        (dist / "index.html").write_text("<!doctype html><title>Radio</title>")
        (dist / "assets" / "app.js").write_text("console.log(1)")
        cls.app = radio_web.App(vault=cls.fx.vault, store_root=cls.fx.store_root, roots=[cls.fx.home / "nonvis"],
                                fake=True, voice=False, env=cls.fx.env(), dist=dist, cast_path=cls.cast_p,
                                lock_path=cls.lock_p, commit_cast=False)
        cls.server = radio_web.make_server(cls.app, "127.0.0.1", 0)
        cls.port = cls.server.server_address[1]
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.tmp.cleanup()

    def req(self, method, path, body=None, headers=None):
        data = json.dumps(body).encode() if body is not None else None
        h = {"Content-Type": "application/json"} if body is not None else {}
        h.update(headers or {})
        r = urllib.request.Request("http://127.0.0.1:%d%s" % (self.port, path), data=data, method=method, headers=h)
        try:
            with urllib.request.urlopen(r, timeout=60) as resp:
                return resp.status, dict(resp.headers), resp.read()
        except urllib.error.HTTPError as e:
            with e:
                return e.code, dict(e.headers), e.read()

    def j(self, method, path, body=None, **kw):
        s, h, b = self.req(method, path, body, **kw)
        return s, json.loads(b or b"null")

    def events(self, job):
        s, h, b = self.req("GET", "/api/jobs/%s/events" % job)
        self.assertEqual(s, 200)
        self.assertIn("text/event-stream", h.get("Content-Type", ""))
        return [json.loads(x[6:]) for x in b.decode().split("\n") if x.startswith("data: ")]

    def generate(self, body):
        s, o = self.j("POST", "/api/generate", body)
        self.assertEqual(s, 202, o)
        ev = self.events(o["job"])
        return o["job"], ev

    # ── health, static ─────────────────────────────────────────────────────

    def test_health(self):
        s, o = self.j("GET", "/api/health")
        self.assertEqual(s, 200)
        self.assertTrue(o["ok"])
        self.assertTrue(o["fake"])
        self.assertFalse(o["voice"])

    def test_the_built_app_is_served_with_a_spa_fallback(self):
        s, h, b = self.req("GET", "/")
        self.assertEqual(s, 200)
        self.assertIn(b"<title>Radio</title>", b)
        s, h, b = self.req("GET", "/assets/app.js")
        self.assertEqual(s, 200)
        self.assertIn("javascript", h["Content-Type"])
        s, h, b = self.req("GET", "/episodes/some-id")
        self.assertEqual((s, b"<title>Radio</title>" in b), (200, True))
        s, h, b = self.req("GET", "/assets/../../cast.json")
        self.assertNotEqual(s, 200)

    def test_an_unknown_api_path_is_404_json(self):
        s, o = self.j("GET", "/api/nope")
        self.assertEqual(s, 404)
        self.assertIn("error", o)

    # ── the walls ──────────────────────────────────────────────────────────

    def test_a_foreign_origin_is_refused(self):
        s, o = self.j("POST", "/api/episodes/x/keep", {"kept": True}, headers={"Origin": "http://evil.example"})
        self.assertEqual(s, 403)

    def test_a_foreign_host_header_is_refused(self):
        s, o = self.j("GET", "/api/health", headers={"Host": "evil.example"})
        self.assertEqual(s, 403)

    def test_the_vite_dev_origin_is_allowed(self):
        s, o = self.j("GET", "/api/health", headers={"Origin": "http://localhost:5173"})
        self.assertEqual(s, 200)

    # ── projects ───────────────────────────────────────────────────────────

    def test_projects_list_check_and_toggle(self):
        s, o = self.j("GET", "/api/projects")
        self.assertEqual(s, 200)
        ids = [p["id"] for p in o["projects"]]
        self.assertIn("sidebar", ids)
        self.assertTrue(all("active" in p for p in o["projects"]))
        s, c = self.j("POST", "/api/projects/check", {"id": "sidebar", "name": "S", "notes": ["01_Projects/Sidebar/13 - *.md"],
                                                       "folders": ["01_Projects/Sidebar"], "repos": [], "concept": []})
        self.assertEqual(s, 200)
        self.assertTrue(any("already exists" in e for e in c["errors"]))
        s, r = self.j("PATCH", "/api/projects/meridian", {"active": False})
        self.assertEqual(s, 200, r)
        self.assertFalse(next(p for p in self.j("GET", "/api/projects")[1]["projects"] if p["id"] == "meridian")["active"])
        self.j("PATCH", "/api/projects/meridian", {"active": True})
        s, r = self.j("PATCH", "/api/projects/nope", {"active": True})
        self.assertEqual(s, 404)

    def test_candidates_and_derive(self):
        s, o = self.j("GET", "/api/projects/candidates")
        self.assertEqual(s, 200)
        self.assertIn("repos", o)
        self.assertNotIn("01_Projects/Sidebar", o["folders"])            # taken
        s, d = self.j("POST", "/api/projects/derive", {"folder": "01_Projects/Sidebar"})
        self.assertEqual(s, 200, d)
        self.assertEqual(d["spec"]["id"], "sidebar")
        self.assertTrue(any("already" in e for e in d["check"]["errors"]))
        self.assertEqual(self.j("POST", "/api/projects/derive", {})[0], 400)

    def test_adding_a_project_with_errors_is_400_with_the_errors(self):
        s, o = self.j("POST", "/api/projects", {"id": "x", "name": "X", "notes": ["01_Projects/X/*.md"],
                                                 "folders": ["01_Projects/X"], "repos": [], "concept": []})
        self.assertEqual(s, 400)
        self.assertTrue(o["errors"])

    # ── the cast ───────────────────────────────────────────────────────────

    def test_cast_lists_specialties_and_free_voices(self):
        s, o = self.j("GET", "/api/cast")
        self.assertEqual(s, 200)
        ids = {d["id"]: d for d in o["cast"]}
        self.assertEqual(ids["builder"]["angle"], "What would it take to build, and what already exists?")
        self.assertNotIn("af_heart", o["free_voices"])
        self.assertIn("centre", o["pans"])

    def test_adding_a_specialty(self):
        s, o = self.j("POST", "/api/cast", {"id": "skeptic2", "name": "Contrarian", "voice": "bm_lewis", "pan": "far left",
                                            "angle": "What is the strongest case against it?"})
        self.assertEqual(s, 200, o)
        self.assertIn("skeptic2", [d["id"] for d in self.j("GET", "/api/cast")[1]["cast"]])
        s, o = self.j("POST", "/api/cast", {"id": "other", "name": "O", "voice": "bm_lewis", "pan": "left",
                                            "angle": "Why?"})
        self.assertEqual(s, 400)

    # ── episodes ───────────────────────────────────────────────────────────

    def test_generate_a_recap_then_play_keep_export_and_delete(self):
        job, ev = self.generate({"kind": "recap", "project": "sidebar", "range": "today"})
        kinds = [e["event"] for e in ev]
        self.assertEqual(kinds[0], "started")
        self.assertIn("line", kinds)
        self.assertEqual(kinds[-1], "exit")
        self.assertEqual(ev[-1]["code"], 0)
        eid = ev[0]["id"]
        s, o = self.j("GET", "/api/episodes")
        self.assertIn(eid, [m["id"] for m in o["episodes"]])
        s, o = self.j("GET", "/api/episodes/%s" % eid)
        self.assertEqual(o["meta"]["status"], "unvoiced")
        self.assertTrue(o["script"]["lines"])
        s, h, b = self.req("GET", "/api/episodes/%s/script.md?download=1" % eid)
        self.assertEqual(s, 200)
        self.assertIn("attachment", h["Content-Disposition"])
        self.assertIn(b"## Script", b)
        s, o = self.j("POST", "/api/episodes/%s/keep" % eid, {"kept": True})
        self.assertTrue(o["kept"])
        s, o = self.j("DELETE", "/api/episodes/%s" % eid)
        self.assertEqual(s, 200)
        self.assertEqual(self.j("GET", "/api/episodes/%s" % eid)[0], 404)

    def test_generate_a_brainstorm_with_context(self):
        first = [m for m in self.j("GET", "/api/episodes")[1]["episodes"] if m["title"] == "Sidebar (2026-10-15)"][0]["id"]
        job, ev = self.generate({"kind": "brainstorm", "project": "sidebar", "question": "Should the gate run first?",
                                 "specialties": ["builder", "product"], "context": [first]})
        self.assertEqual(ev[-1]["code"], 0, ev)
        m = self.j("GET", "/api/episodes/%s" % ev[0]["id"])[1]["meta"]
        self.assertEqual((m["kind"], m["context"]), ("brainstorm", [first]))
        self.assertTrue([e for e in ev if e["event"] == "room"])                       # the room's turns stream (QR-34)
        self.assertIn("editing", [e.get("status") for e in ev if e["event"] == "status"])
        s, h, b = self.req("GET", "/api/episodes/%s/room.md?download=1" % ev[0]["id"])
        self.assertEqual(s, 200)
        self.assertIn(b"# Room: Should the gate run first?", b)
        self.assertIn("-room.md", h["Content-Disposition"])

    def test_generate_refusals(self):
        self.assertEqual(self.j("POST", "/api/generate", {"kind": "recap", "project": "nope"})[0], 400)
        self.assertEqual(self.j("POST", "/api/generate", {"kind": "brainstorm", "project": "sidebar", "question": ""})[0], 400)
        self.assertEqual(self.j("POST", "/api/generate", {"kind": "podcast", "project": "sidebar"})[0], 400)

    def test_a_refused_recap_ends_its_job_with_the_reason(self):
        job, ev = self.generate({"kind": "recap", "project": "meridian", "range": "today"})
        self.assertEqual(ev[-1]["event"], "exit")
        self.assertNotEqual(ev[-1]["code"], 0)
        self.assertTrue(any(e["event"] == "error" and "nothing on meridian" in e["message"] for e in ev))

    def test_audio_is_served_with_ranges(self):
        st = __import__("radio_store").Store(self.fx.store_root)
        eid = st.new("recap", "quorum", NOW, {"title": "Quorum (2026-10-17)", "status": "ready"})
        st.audio_path(eid).write_bytes(bytes(range(256)) * 4)
        s, h, b = self.req("GET", "/api/episodes/%s/audio" % eid)
        self.assertEqual((s, len(b), h["Accept-Ranges"]), (200, 1024, "bytes"))
        self.assertEqual(h["Content-Type"], "audio/mpeg")
        s, h, b = self.req("GET", "/api/episodes/%s/audio" % eid, headers={"Range": "bytes=10-19"})
        self.assertEqual((s, b, h["Content-Range"]), (206, bytes(range(10, 20)), "bytes 10-19/1024"))
        s, h, b = self.req("GET", "/api/episodes/%s/audio" % eid, headers={"Range": "bytes=1000-"})
        self.assertEqual((s, len(b)), (206, 24))
        s, h, b = self.req("GET", "/api/episodes/%s/audio" % eid, headers={"Range": "bytes=5000-"})
        self.assertEqual(s, 416)
        st.delete(eid)

    def test_a_bad_episode_id_is_404(self):
        for p in ("/api/episodes/..%2F..%2Fetc", "/api/episodes/nope", "/api/episodes/nope/audio"):
            self.assertEqual(self.req("GET", p)[0], 404, p)


if __name__ == "__main__":
    unittest.main()
