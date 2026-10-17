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
        cls.roles = root / "roles"
        shutil.copytree(radio_web.radio.ROLES_DIR, cls.roles)
        cls.app = radio_web.App(vault=cls.fx.vault, store_root=cls.fx.store_root, roots=[cls.fx.home / "nonvis"],
                                fake=True, voice=False, env=cls.fx.env(), dist=dist, cast_path=cls.cast_p,
                                lock_path=cls.lock_p, commit_cast=False, roles_dir=cls.roles)
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

    def brainstorm(self):
        job, ev = self.generate({"kind": "brainstorm", "project": "sidebar", "question": "Gate first?",
                                 "specialties": ["builder", "product"]})
        return ev[0]["id"]

    def test_feedback_roles_and_replay(self):
        """The tuning loop: vote on a room turn, read it on the specialty's role page, edit the role, replay the turn."""
        eid = self.brainstorm()
        sc = self.j("GET", "/api/episodes/%s" % eid)[1]["script"]
        turn = next(t for t in sc["room"]["turns"] if t["speaker"] == "builder" and t["kind"] == "answer")
        s, f = self.j("POST", "/api/episodes/%s/feedback" % eid,
                      {"target": "turn", "n": turn["n"], "vote": "down", "note": "didn't read the code"})
        self.assertEqual((s, f["speaker"]), (200, "builder"))                           # the speaker comes from the episode
        line = next(l for l in sc["lines"] if l["speaker"] == "product")
        self.assertEqual(self.j("POST", "/api/episodes/%s/feedback" % eid, {"target": "line", "n": line["i"], "vote": "up"})[0], 200)
        self.assertEqual(self.j("POST", "/api/episodes/%s/feedback" % eid, {"target": "turn", "n": 999, "vote": "up"})[0], 400)
        self.assertEqual(len(self.j("GET", "/api/episodes/%s/feedback" % eid)[1]["feedback"]), 2)

        s, roles = self.j("GET", "/api/roles")
        b = next(r for r in roles["roles"] if r["id"] == "builder")
        self.assertGreaterEqual(b["down"], 1)
        s, role = self.j("GET", "/api/roles/builder")
        self.assertIn("## Method", role["text"])
        fb = next(x for x in role["feedback"] if x["episode"] == eid)
        self.assertEqual((fb["note"], fb["excerpt"][:20]), ("didn't read the code", turn["text"][:20]))

        s, r = self.j("POST", "/api/roles/builder/replay", {"episode": eid, "turn": turn["n"], "text": "Read first, always."})
        self.assertEqual(s, 200, r)
        self.assertEqual(r["old"], turn["text"])
        self.assertTrue(r["new"])
        self.assertEqual(self.j("POST", "/api/roles/product/replay", {"episode": eid, "turn": turn["n"], "text": "x"})[0], 400)
        # A room kept before its dirs were recorded reads the project's dirs from the vault (real replay 10-17).
        self.assertEqual(self.app.project_dirs("sidebar"),
                         [str(self.fx.home / "nonvis" / "sidebar"), str(self.fx.home / "nonvis" / "sidebar-screen"),
                          str(self.fx.vault / "01_Projects/Sidebar")])

        s, r = self.j("PUT", "/api/roles/builder", {"text": "# Builder\n\nRead first, always.\n"})
        self.assertEqual(s, 200, r)
        self.assertEqual((self.roles / "builder.md").read_text(), "# Builder\n\nRead first, always.\n")
        self.assertEqual(self.j("PUT", "/api/roles/nobody", {"text": "x"})[0], 404)
        self.assertEqual(self.j("PUT", "/api/roles/builder", {"text": ""})[0], 400)
        fid = self.j("GET", "/api/episodes/%s/feedback" % eid)[1]["feedback"][0]["id"]
        self.assertEqual(self.j("DELETE", "/api/episodes/%s/feedback/%s" % (eid, fid))[0], 200)

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


class StudyWebCase(unittest.TestCase):
    """Study (QR-41…47) over the API: a track over a work repo, a day, the close, finish + delete.
    The same server as WebCase, without re-running its tests."""
    req, j, events = WebCase.req, WebCase.j, WebCase.events
    tearDownClass = classmethod(WebCase.tearDownClass.__func__)

    @classmethod
    def setUpClass(cls):
        WebCase.setUpClass.__func__(cls)
        cls.repo = cls.fx.home / "projects" / "work" / "sweeping-sui"
        (cls.repo / "listener" / "src").mkdir(parents=True)
        (cls.repo / "listener" / "src" / "main.rs").write_text("fn main() {\n    run();\n}\n")
        git(cls.repo, "init", "-q", "-b", "main")
        cls.app.study = radio_web.study.Store(Path(cls.tmp.name) / "study")
        cls.app.study_roots = [cls.fx.home / "nonvis", cls.fx.home / "projects"]
        cls.app.env["QUORUM_STUDY_STORE"] = str(cls.app.study.root)

    def run_job(self, path, body):
        s, o = self.j("POST", path, body)
        self.assertEqual(s, 202, o)
        return self.events(o["job"])

    def test_a_track_day_close_finish_delete(self):
        s, o = self.j("GET", "/api/study/candidates")
        self.assertEqual(s, 200)
        self.assertIn(str(self.repo.resolve()), [c["path"] for c in o["repos"]])
        s, o = self.j("POST", "/api/study", {"id": "web-listener", "project": "bastion", "repo": str(self.repo),
                                             "subtree": "listener", "goal": "own the listener"})
        self.assertEqual(s, 200, o)
        s, o = self.j("POST", "/api/study", {"id": "bad", "project": "bastion", "repo": "/tmp", "goal": "g"})
        self.assertEqual(s, 400)

    def test_one_pick_creates_a_track(self):
        s, o = self.j("GET", "/api/study/candidates")
        pick = next(c for c in o["repos"] if c["name"] == "sweeping-sui" and c["subtree"] == "listener")
        self.assertIn("touched", pick)
        s, t = self.j("POST", "/api/study", {"repo": pick["path"], "subtree": pick["subtree"]})
        self.assertEqual(s, 200, t)
        self.assertEqual((t["id"], t["project"], t["subtree"], t["goal"]),
                         ("sweeping-sui-listener", "work", "listener", "Understand sweeping-sui/listener"))
        s, t2 = self.j("POST", "/api/study", {"repo": pick["path"], "subtree": "listener", "goal": "own the V2 listener"})
        self.assertEqual((t2["id"], t2["goal"]), ("sweeping-sui-listener-2", "own the V2 listener"))

        ev = self.run_job("/api/study/web-listener/say", {"message": "where does it start?"})
        kinds = [e["event"] for e in ev]
        self.assertEqual(kinds[0], "started")
        self.assertIn("tool", kinds)
        self.assertIn("turn", kinds)
        self.assertEqual(kinds[-1], "exit")
        self.assertEqual(ev[-1]["code"], 0)
        s, h = self.j("GET", "/api/health")
        self.assertEqual(h["running"], [])                          # a study job is never a generation

        s, o = self.j("GET", "/api/study/web-listener")
        self.assertEqual(s, 200)
        [day] = o["days"]
        self.assertEqual(day["status"], "open")
        s, d = self.j("GET", "/api/study/web-listener/days/%s" % day["date"])
        self.assertEqual([t["who"] for t in d["turns"]], ["sang", "tutor"])
        self.assertTrue(d["turns"][1]["citations"])

        s, e = self.j("GET", "/api/study/web-listener/file?path=listener/src/main.rs&line=2")
        self.assertEqual(s, 200, e)
        self.assertIn([2, "    run();"], e["lines"])
        for bad in ("../../../etc/passwd", ".git/HEAD"):
            self.assertEqual(self.j("GET", "/api/study/web-listener/file?path=%s&line=1" % bad)[0], 400)

        ev = self.run_job("/api/study/web-listener/end", {})
        self.assertIn("closed", [e["event"] for e in ev])
        s, o = self.j("GET", "/api/study/web-listener")
        self.assertTrue(o["track"]["state"]["left_off"])

        self.assertEqual(self.j("DELETE", "/api/study/web-listener")[0], 400)   # only a finished track
        s, o = self.j("POST", "/api/study/web-listener/finish", {})
        self.assertEqual((s, o["status"]), (200, "finished"))
        self.assertEqual(self.j("POST", "/api/study/web-listener/say", {"message": "x"})[0], 400)
        self.assertEqual(self.j("DELETE", "/api/study/web-listener")[0], 200)
        self.assertEqual(self.j("GET", "/api/study/web-listener")[0], 404)

    def test_an_open_earlier_day_is_409(self):
        self.j("POST", "/api/study", {"id": "web-open", "project": "bastion", "repo": str(self.repo), "goal": "g"})
        self.app.study.save_day("web-open", {"v": 1, "date": "2026-10-01", "session": "x", "started": True,
                                             "status": "open", "turns": [], "cost": 0, "close": None})
        s, o = self.j("POST", "/api/study/web-open/say", {"message": "hi"})
        self.assertEqual((s, o.get("open_day")), (409, "2026-10-01"))
        s, o = self.j("POST", "/api/study/web-open/start", {})
        self.assertEqual((s, o.get("open_day")), (409, "2026-10-01"))

    def test_start_opens_today_without_a_message(self):
        self.j("POST", "/api/study", {"id": "web-start", "project": "bastion", "repo": str(self.repo), "goal": "g"})
        ev = self.run_job("/api/study/web-start/start", {})
        self.assertEqual(ev[-1]["code"], 0, ev)
        [d] = self.app.study.days("web-start")
        self.assertEqual([t["who"] for t in d["turns"]], ["tutor"])

    def test_a_bad_track_id_is_404(self):
        for p in ("/api/study/..%2F..%2Fetc", "/api/study/nope", "/api/study/nope/days/2026-10-01"):
            self.assertEqual(self.req("GET", p)[0], 404, p)


if __name__ == "__main__":
    unittest.main()
