#!/usr/bin/env python3
"""Tests for study.py — Study (QR-41…47): a track, one session a day, the close as the
memory, the next day loading the summary only, finish + delete. Real temp dirs, the fake tutor."""
import json
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import study  # noqa: E402

DAY1 = datetime(2026, 10, 18, 1, 0, tzinfo=timezone.utc)     # 10:00 KST, 10-18
DAY1_LATE = DAY1 + timedelta(hours=2)
DAY2 = DAY1 + timedelta(days=1)


def git(d, *args):
    subprocess.run(["git", "-C", str(d)] + list(args), check=True, capture_output=True)


class Case(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.home = root / "home"
        self.repo = self.home / "projects" / "work" / "sweeping-sui"
        (self.repo / "listener" / "src").mkdir(parents=True)
        (self.repo / "listener" / "src" / "main.rs").write_text("fn main() {\n    run();\n}\n")
        (self.repo / "listener" / "src" / "poll.rs").write_text("pub fn poll() {}\n")
        (self.repo / "README.md").write_text("# sweeping-sui\n")
        git(self.repo, "init", "-q", "-b", "main")
        git(self.repo, "-c", "user.email=t@t", "-c", "user.name=t", "add", "-A")
        git(self.repo, "-c", "user.email=t@t", "-c", "user.name=t", "commit", "-qm", "init")
        self.roots = [self.home / "nonvis", self.home / "projects"]
        self.store = study.Store(root / "store")
        self.model = study.FakeTutor()

    def tearDown(self):
        self.tmp.cleanup()

    def new(self, tid="listener"):
        return self.store.create(tid, project="bastion", repo=str(self.repo), subtree="listener",
                                 goal="own the V2 listener", now=DAY1, roots=self.roots)

    def tutor_calls(self):
        return [c for c in self.model.calls if c["kind"] == "tutor"]


class TrackTest(Case):
    def test_create_checks_repo_and_id(self):
        t = self.new()
        self.assertEqual(t["status"], "active")
        self.assertEqual(t["repo"], str(self.repo.resolve()))
        with self.assertRaises(study.StudyError):
            self.new()                                            # the id is taken
        with self.assertRaises(study.StudyError):
            self.store.create("Bad Id", "bastion", str(self.repo), "g", now=DAY1, roots=self.roots)
        with self.assertRaises(study.StudyError):                 # not under ~/nonvis or ~/projects
            self.store.create("x", "bastion", "/tmp", "g", now=DAY1, roots=self.roots)
        with self.assertRaises(study.StudyError):
            self.store.create("y", "bastion", str(self.repo), "g", subtree="../..", now=DAY1, roots=self.roots)
        with self.assertRaises(study.StudyError):
            self.store.create("z", "bastion", str(self.repo), "", now=DAY1, roots=self.roots)

    def test_a_track_is_never_indexed_or_allowlisted(self):
        self.new()
        self.assertFalse((self.repo / ".quorum").exists())
        study.start(self.store, "listener", self.model, DAY1, message="where does it start?")
        study.end(self.store, "listener", self.model, DAY1_LATE)
        self.assertFalse((self.repo / ".quorum").exists())
        self.assertEqual(subprocess.run(["git", "-C", str(self.repo), "status", "--porcelain"],
                                        capture_output=True, text=True).stdout, "")

    def test_one_pick_derives_id_project_and_goal(self):
        d = study.derive(self.store, str(self.repo), "listener", projects={"bastion": []}, roots=self.roots)
        self.assertEqual(d, {"id": "sweeping-sui-listener", "project": "work", "repo": str(self.repo.resolve()),
                             "subtree": "listener", "goal": "Understand sweeping-sui/listener"})
        d = study.derive(self.store, str(self.repo), None, projects={"bastion": ["sweeping-sui"]}, roots=self.roots)
        self.assertEqual((d["id"], d["project"], d["goal"]), ("sweeping-sui", "bastion", "Understand sweeping-sui"))
        self.store.create("sweeping-sui-listener", "work", str(self.repo), "g", now=DAY1, roots=self.roots)
        d = study.derive(self.store, str(self.repo), "listener", projects={}, roots=self.roots)
        self.assertEqual(d["id"], "sweeping-sui-listener-2")     # a taken id gets a suffix

    def test_the_parent_folder_names_the_project_when_it_is_one(self):
        d = study.derive(self.store, str(self.repo), None, projects={"work": []}, roots=self.roots)
        self.assertEqual(d["project"], "work")
        flat = self.home / "nonvis" / "toolbox"
        flat.mkdir(parents=True)
        git(flat, "init", "-q")
        self.assertEqual(study.derive(self.store, str(flat), None, projects={}, roots=self.roots)["project"], "toolbox")

    def test_candidates_are_repos_and_their_folders_newest_first(self):
        old = self.home / "projects" / "work" / "old-listener"
        (old / "indexer").mkdir(parents=True)
        (old / "README.md").write_text("# Old Listener\n\nThe retired V1 listener.\n")
        git(old, "init", "-q", "-b", "main")
        os_t = 1_700_000_000
        import os
        for p in (old / ".git").rglob("*"):
            os.utime(p, (os_t, os_t))
        os.utime(old / ".git", (os_t, os_t))
        c = study.candidates(self.roots)
        paths = [(x["name"], x["subtree"]) for x in c]
        self.assertLess(paths.index(("sweeping-sui", None)), paths.index(("old-listener", None)))
        self.assertIn(("sweeping-sui", "listener"), paths)
        self.assertNotIn(("sweeping-sui", ".git"), paths)
        o = next(x for x in c if x["name"] == "old-listener" and not x["subtree"])
        self.assertEqual(o["summary"], "The retired V1 listener.")
        (old / "indexer" / "README.md").write_text("---\ntitle: Indexer\n---\n# Indexer\n\nReads checkpoints.\n")
        self.assertEqual(study._summary(old / "indexer"), "Reads checkpoints.")
        (old / "indexer" / "README.md").write_text("# I\n\n> [!WARNING]\n> Deprecated: see v2.\n")
        self.assertEqual(study._summary(old / "indexer"), "Deprecated: see v2.")
        self.assertEqual(o["head"]["ref"], "main")
        self.assertTrue(o["touched"])

    def test_head_is_read_from_the_git_dir_without_git(self):
        self.assertEqual(study.read_head(self.repo)["ref"], "main")
        self.assertEqual(len(study.read_head(self.repo)["sha"]), 40)
        self.assertIsNone(study.read_head(self.home))


class DayTest(Case):
    def test_day_one_starts_from_nothing(self):
        self.new()
        d = study.start(self.store, "listener", self.model, DAY1)
        self.assertEqual(d["date"], "2026-10-18")
        call = self.tutor_calls()[0]
        self.assertFalse(call["started"])
        self.assertIn("own the V2 listener", call["user"])
        self.assertIn("Day 1", call["user"])
        self.assertNotIn("Left off", call["user"])
        self.assertEqual([t["who"] for t in d["turns"]], ["tutor"])
        self.assertIn("Read", call["system"])

    def test_same_day_resumes_the_same_session(self):
        self.new()
        d = study.start(self.store, "listener", self.model, DAY1, message="what is poll?")
        study.say(self.store, "listener", "and main?", self.model, DAY1_LATE)
        a, b = self.tutor_calls()
        self.assertEqual(a["session"], b["session"])
        self.assertFalse(a["started"])
        self.assertTrue(b["started"])
        day = self.store.day("listener", d["date"])
        self.assertEqual([t["who"] for t in day["turns"]], ["sang", "tutor", "sang", "tutor"])
        self.assertGreater(day["cost"], 0)

    def test_say_with_no_day_starts_one(self):
        self.new()
        study.say(self.store, "listener", "first question", self.model, DAY1)
        self.assertEqual(len(self.store.days("listener")), 1)
        self.assertIn("first question", self.tutor_calls()[0]["user"])

    def test_the_close_is_the_memory(self):
        self.new()
        study.start(self.store, "listener", self.model, DAY1, message="walk me through main")
        c = study.end(self.store, "listener", self.model, DAY1_LATE)
        self.assertTrue(c["left_off"])
        t = self.store.track("listener")
        self.assertEqual(t["state"]["left_off"], c["left_off"])
        self.assertIn("listener/src/main.rs", [x["path"] for x in t["state"]["covered"]])
        self.assertIn("listener/src/main.rs", t["files"])
        self.assertEqual(t["head"]["ref"], "main")
        day = self.store.day("listener", "2026-10-18")
        self.assertEqual(day["status"], "closed")
        self.assertEqual(day["close"], c)
        close_call = [x for x in self.model.calls if x["kind"] == "close"][0]
        self.assertTrue(close_call["started"])                    # the same session writes it

    def test_next_day_loads_the_summary_never_the_transcript(self):
        self.new()
        study.start(self.store, "listener", self.model, DAY1, message="SECRET-PHRASE-IN-DAY-ONE")
        study.end(self.store, "listener", self.model, DAY1_LATE)
        d2 = study.start(self.store, "listener", self.model, DAY2)
        call = self.tutor_calls()[-1]
        self.assertFalse(call["started"])
        self.assertNotEqual(call["session"], self.store.day("listener", "2026-10-18")["session"])
        self.assertIn("Left off", call["user"])
        self.assertIn("listener/src/main.rs", call["user"])
        self.assertIn("Day 2", call["user"])
        self.assertNotIn("SECRET-PHRASE-IN-DAY-ONE", call["user"])
        self.assertEqual(d2["date"], "2026-10-19")

    def test_only_the_last_three_days_are_loaded(self):
        self.new()
        for k in range(5):
            now = DAY1 + timedelta(days=k)
            study.start(self.store, "listener", self.model, now, message="day %d" % k)
            self.model.left_off = "LEFT-OFF-%d" % k
            study.end(self.store, "listener", self.model, now + timedelta(hours=1))
        study.start(self.store, "listener", self.model, DAY1 + timedelta(days=5))
        user = self.tutor_calls()[-1]["user"]
        self.assertNotIn("LEFT-OFF-0", user)
        self.assertNotIn("LEFT-OFF-1", user)
        for k in (2, 3, 4):
            self.assertIn("LEFT-OFF-%d" % k, user)

    def test_an_unended_day_blocks_the_next(self):
        self.new()
        study.start(self.store, "listener", self.model, DAY1, message="hi")
        with self.assertRaises(study.OpenDay) as e:
            study.start(self.store, "listener", self.model, DAY2)
        self.assertEqual(e.exception.date, "2026-10-18")
        study.end(self.store, "listener", self.model, DAY2)        # ends the open day, not today
        self.assertEqual(self.store.day("listener", "2026-10-18")["status"], "closed")
        study.start(self.store, "listener", self.model, DAY2)

    def test_a_closed_day_reopens_the_same_day(self):
        self.new()
        study.start(self.store, "listener", self.model, DAY1, message="a")
        study.end(self.store, "listener", self.model, DAY1_LATE)
        study.say(self.store, "listener", "one more", self.model, DAY1_LATE)
        day = self.store.day("listener", "2026-10-18")
        self.assertEqual(day["status"], "open")
        self.assertTrue(self.tutor_calls()[-1]["started"])

    def test_unclear_carries_and_covered_merges(self):
        self.new()
        study.start(self.store, "listener", self.model, DAY1, message="a")
        self.model.unclear = ["why two pollers?"]
        study.end(self.store, "listener", self.model, DAY1_LATE)
        study.start(self.store, "listener", self.model, DAY2, message="b")
        self.model.covered = [{"path": "listener/src/poll.rs", "what": "the poll loop"}]
        self.model.unclear = []
        study.end(self.store, "listener", self.model, DAY2 + timedelta(hours=1))
        close_user = [x for x in self.model.calls if x["kind"] == "close"][-1]["user"]
        self.assertIn("why two pollers?", close_user)              # the open questions reach the close
        st = self.store.track("listener")["state"]
        self.assertEqual(sorted(x["path"] for x in st["covered"]), ["listener/src/main.rs", "listener/src/poll.rs"])
        self.assertEqual(st["unclear"], [])

    def test_a_bad_close_keeps_the_day_open(self):
        self.new()
        study.start(self.store, "listener", self.model, DAY1, message="a")
        self.model.bad_close = 2
        with self.assertRaises(study.StudyError):
            study.end(self.store, "listener", self.model, DAY1_LATE)
        self.assertEqual(self.store.day("listener", "2026-10-18")["status"], "open")
        self.assertEqual(self.store.track("listener")["state"]["left_off"], "")
        self.model.bad_close = 1                                   # one bad reply, then the retry lands
        study.end(self.store, "listener", self.model, DAY1_LATE)
        self.assertEqual(self.store.day("listener", "2026-10-18")["status"], "closed")

    def test_changed_files_are_named_next_day(self):
        self.new()
        study.start(self.store, "listener", self.model, DAY1, message="a")
        study.end(self.store, "listener", self.model, DAY1_LATE)
        self.assertEqual(study.changed_files(self.store.track("listener")), [])
        (self.repo / "listener" / "src" / "main.rs").write_text("fn main() { other(); }\n")
        self.assertEqual(study.changed_files(self.store.track("listener")), ["listener/src/main.rs"])
        study.start(self.store, "listener", self.model, DAY2)
        self.assertIn("Changed since", self.tutor_calls()[-1]["user"])

    def test_tool_events_reach_the_listener(self):
        self.new()
        seen = []
        study.start(self.store, "listener", self.model, DAY1, message="a", on_event=seen.append)
        self.assertIn("tool", [e["event"] for e in seen])
        self.assertIn("turn", [e["event"] for e in seen])
        day = self.store.day("listener", "2026-10-18")
        self.assertTrue(day["turns"][-1]["tools"])


class FinishTest(Case):
    def test_finish_then_delete(self):
        self.new()
        study.start(self.store, "listener", self.model, DAY1, message="a")
        with self.assertRaises(study.StudyError):
            self.store.delete("listener")                          # only a finished track is deleted
        t = study.finish(self.store, "listener", self.model, DAY1_LATE)
        self.assertEqual(t["status"], "finished")
        self.assertEqual(self.store.day("listener", "2026-10-18")["status"], "closed")   # the open day closed first
        with self.assertRaises(study.StudyError):
            study.start(self.store, "listener", self.model, DAY2)
        self.store.delete("listener")
        self.assertEqual(self.store.tracks(), [])

    def test_an_active_track_is_deleted_when_asked_outright(self):
        self.new()
        study.start(self.store, "listener", self.model, DAY1, message="a")      # an open day, never closed
        self.store.delete("listener", active_ok=True)
        self.assertEqual(self.store.tracks(), [])
        self.assertFalse(self.repo.joinpath(".quorum").exists())

    def test_tracks_list(self):
        self.new()
        study.start(self.store, "listener", self.model, DAY1, message="a")
        [t] = self.store.tracks()
        self.assertEqual(t["id"], "listener")
        self.assertEqual(t["days"], 1)
        self.assertEqual(t["open_day"], "2026-10-18")


class ExcerptTest(Case):
    def test_excerpt_and_walls(self):
        t = self.new()
        e = study.excerpt(t, "listener/src/main.rs", 2)
        self.assertEqual(e["path"], "listener/src/main.rs")
        self.assertIn([2, "    run();"], e["lines"])
        self.assertEqual(study.excerpt(t, "src/main.rs", 1)["path"], "listener/src/main.rs")   # relative to the focus
        for bad in ("../../../etc/passwd", "/etc/passwd", ".git/config"):
            with self.assertRaises(study.StudyError):
                study.excerpt(t, bad, 1)

    def test_shortened_citations_resolve_by_suffix_and_the_nearest_dir(self):
        t = self.new()
        (self.repo / "listener" / "src" / "main.rs").unlink()      # an exact match under the focus would win
        for rel in ("listener/indexer/src/main.rs", "listener/indexer/src/runtime/mod.rs", "listener/indexer/src/grpc/mod.rs"):
            (self.repo / rel).parent.mkdir(parents=True, exist_ok=True)
            (self.repo / rel).write_text("a\nb\n")
        got = study.resolve_citations(t, "see `src/main.rs:1`, `src/runtime/mod.rs:2`, then `mod.rs:1`; `nope.rs:3`")
        self.assertEqual([(c["path"], c["resolved"]) for c in got], [
            ("src/main.rs", "listener/indexer/src/main.rs"),
            ("src/runtime/mod.rs", "listener/indexer/src/runtime/mod.rs"),
            ("mod.rs", "listener/indexer/src/runtime/mod.rs")])

    def test_citations(self):
        self.assertEqual(study.citations("see `listener/src/main.rs:2` and poll.rs:10-12, not v1.2"),
                         [{"path": "listener/src/main.rs", "line": 2, "end": None},
                          {"path": "poll.rs", "line": 10, "end": 12}])


class CliTest(Case):
    def run_cli(self, *args, env_now=DAY1):
        env = {"QUORUM_STUDY_STORE": str(self.store.root), "QUORUM_STUDY_NOW": env_now.strftime("%Y-%m-%dT%H:%M:%SZ"),
               "HOME": str(self.home), "PATH": "/usr/bin:/bin"}
        return subprocess.run([sys.executable, str(HERE / "study.py")] + list(args) + ["--fake"],
                              capture_output=True, text=True, env=env)

    def test_cli_round_trip_with_events(self):
        r = self.run_cli("new", "listener", "--project", "bastion", "--repo", str(self.repo), "--subtree", "listener",
                         "--goal", "own the V2 listener")
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_cli("say", "listener", "where does it start?", "--events")
        self.assertEqual(r.returncode, 0, r.stderr)
        evs = [json.loads(l) for l in r.stdout.splitlines()]
        self.assertEqual(evs[0]["event"], "started")
        self.assertEqual(evs[-1]["event"], "done")
        r = self.run_cli("end", "listener")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Left off", r.stdout)
        r = self.run_cli("say", "listener", "next", env_now=DAY2)
        self.assertEqual(r.returncode, 0, r.stderr)
        r = self.run_cli("list")
        self.assertIn("listener", r.stdout)

    def test_cli_delete_an_active_track_needs_yes(self):
        self.run_cli("new", "listener", "--project", "bastion", "--repo", str(self.repo), "--goal", "g")
        self.assertEqual(self.run_cli("delete", "listener").returncode, 1)
        r = self.run_cli("delete", "listener", "--yes")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.store.tracks(), [])

    def test_cli_open_day_is_refused_with_its_date(self):
        self.run_cli("new", "listener", "--project", "bastion", "--repo", str(self.repo), "--goal", "g")
        self.run_cli("say", "listener", "a")
        r = self.run_cli("say", "listener", "b", "--events", env_now=DAY2)
        self.assertEqual(r.returncode, 3)
        ev = json.loads(r.stdout.splitlines()[-1])
        self.assertEqual(ev["event"], "error")
        self.assertEqual(ev["open_day"], "2026-10-18")


if __name__ == "__main__":
    unittest.main()
