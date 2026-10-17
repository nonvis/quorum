#!/usr/bin/env python3
"""Tests for radio_store.py — the per-host episode store (QR-19, QR-20)."""
import json
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import radio_store  # noqa: E402

NOW = datetime(2026, 10, 17, 5, 0, tzinfo=timezone.utc)


class StoreCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "radio"
        self.s = radio_store.Store(self.root)

    def tearDown(self):
        self.tmp.cleanup()

    def new(self, kind="recap", project="sidebar", when=NOW, **meta):
        return self.s.new(kind, project, when, dict({"title": "Sidebar (2026-10-17)"}, **meta))

    def test_an_episode_is_a_folder_named_by_time_kind_and_project(self):
        i = self.new()
        self.assertEqual(i, "20261017-140000-recap-sidebar")   # KST in the id
        m = self.s.meta(i)
        self.assertEqual(m["kind"], "recap")
        self.assertEqual(m["project"], "sidebar")
        self.assertEqual(m["status"], "generating")
        self.assertFalse(m["kept"])
        self.assertEqual(m["created"], "2026-10-17T05:00:00Z")
        self.assertTrue((self.root / i / "meta.json").exists())

    def test_two_episodes_in_the_same_second_get_distinct_ids(self):
        a, b = self.new(), self.new()
        self.assertNotEqual(a, b)
        self.assertTrue(b.startswith(a))

    def test_an_unknown_kind_or_a_bad_project_is_refused(self):
        with self.assertRaises(ValueError):
            self.new(kind="podcast")
        with self.assertRaises(ValueError):
            self.new(project="../etc")

    def test_ids_that_are_not_ids_are_refused(self):
        for bad in ("../x", "20261017-140000-recap-sidebar/../../x", "", "x"):
            with self.assertRaises(KeyError):
                self.s.meta(bad)

    def test_the_script_is_written_as_json_and_markdown(self):
        i = self.new()
        self.s.write_script(i, {"lines": [{"i": 0, "text": "hi"}]}, "# Sidebar\n")
        self.assertEqual(self.s.script(i)["lines"][0]["text"], "hi")
        self.assertEqual(self.s.script_md(i), "# Sidebar\n")

    def test_update_merges_fields(self):
        i = self.new()
        self.s.update(i, status="ready", duration=12.5)
        m = self.s.meta(i)
        self.assertEqual((m["status"], m["duration"], m["kind"]), ("ready", 12.5, "recap"))

    def test_list_is_newest_first_with_days_left(self):
        a = self.new(when=NOW - timedelta(days=2))
        b = self.new(when=NOW)
        got = self.s.list(NOW)
        self.assertEqual([m["id"] for m in got], [b, a])
        self.assertEqual(got[1]["expires"], "2026-10-22T05:00:00Z")
        self.assertEqual(got[0]["days_left"], 7)
        self.assertEqual(got[1]["days_left"], 5)

    def test_an_unkept_episode_is_swept_after_seven_days_a_kept_one_stays(self):
        old = self.new(when=NOW - timedelta(days=7, seconds=1))
        kept = self.new(when=NOW - timedelta(days=30), project="meridian")
        self.s.keep(kept, True)
        fresh = self.new(when=NOW - timedelta(days=6))
        got = [m["id"] for m in self.s.list(NOW)]
        self.assertEqual(got, [fresh, kept])
        self.assertFalse((self.root / old).exists())
        self.assertIsNone(self.s.list(NOW)[1]["days_left"])

    def test_unkeep_restarts_nothing_the_clock_is_creation(self):
        i = self.new(when=NOW - timedelta(days=8))
        self.s.keep(i, True)
        self.s.keep(i, False)
        self.assertEqual(self.s.list(NOW), [])

    def test_delete_is_immediate(self):
        i = self.new()
        self.s.delete(i)
        self.assertFalse((self.root / i).exists())
        with self.assertRaises(KeyError):
            self.s.delete(i)

    def test_a_folder_without_meta_is_ignored_not_crashed_on(self):
        (self.root / "20261017-140000-recap-x").mkdir(parents=True)
        (self.root / "notes.txt").write_text("x")
        self.assertEqual(self.s.list(NOW), [])

    def test_a_generating_episode_whose_process_is_gone_reads_as_failed(self):
        i = self.new(pid=99999999)
        self.assertEqual(self.s.list(NOW)[0]["status"], "failed")
        j = self.new(pid=__import__("os").getpid())
        self.assertEqual(next(m for m in self.s.list(NOW) if m["id"] == j)["status"], "generating")

    def test_audio_path_only_when_present(self):
        i = self.new()
        self.assertIsNone(self.s.audio(i))
        (self.root / i / "audio.mp3").write_bytes(b"ID3")
        self.assertEqual(self.s.audio(i), self.root / i / "audio.mp3")

    def test_feedback_on_a_turn_or_a_line(self):
        """The tuning loop: 👍/👎 + a note on a room turn or a broadcast line, kept with the episode."""
        i = self.new()
        a = self.s.add_feedback(i, {"target": "turn", "n": 3, "speaker": "builder", "vote": "down",
                                    "note": "didn't read the code"}, NOW)
        self.s.add_feedback(i, {"target": "line", "n": 2, "speaker": "product", "vote": "up"}, NOW)
        fb = self.s.feedback(i)
        self.assertEqual([(f["target"], f["n"], f["vote"]) for f in fb], [("turn", 3, "down"), ("line", 2, "up")])
        self.assertEqual(fb[0]["at"], "2026-10-17T05:00:00Z")
        self.s.remove_feedback(i, a["id"])
        self.assertEqual(len(self.s.feedback(i)), 1)

    def test_feedback_is_checked(self):
        i = self.new()
        for bad in ({"target": "page", "n": 1, "speaker": "builder", "vote": "up"},
                    {"target": "turn", "n": "x", "speaker": "builder", "vote": "up"},
                    {"target": "turn", "n": 1, "speaker": "builder", "vote": "meh"},
                    {"target": "turn", "n": 1, "speaker": "", "vote": "up"}):
            with self.assertRaises(ValueError):
                self.s.add_feedback(i, bad, NOW)

    def test_the_default_root_is_the_checkouts_dot_quorum(self):
        self.assertEqual(radio_store.default_root({}), radio_store.REPO / ".quorum" / "radio")
        self.assertEqual(radio_store.default_root({"QUORUM_RADIO_STORE": "/x/y"}), Path("/x/y"))

    def test_meta_writes_are_atomic_json(self):
        i = self.new()
        json.loads((self.root / i / "meta.json").read_text())
        self.assertFalse(list((self.root / i).glob("*.tmp")))


if __name__ == "__main__":
    unittest.main()
