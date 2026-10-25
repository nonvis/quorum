#!/usr/bin/env python3
"""Tests for scripts/curate.py. Run: python3 scripts/curate_test.py"""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import curate  # noqa: E402


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def use(tool, path, ts):
    return json.dumps({"type": "assistant", "timestamp": ts, "message": {"content": [
        {"type": "tool_use", "name": tool, "input": {"file_path": str(path)}}]}}) + "\n"


class CurateCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.mem = root / "memory"
        self.tdir = root / "transcripts"
        write(self.mem / "MEMORY.md", "# Memory\n- [Read one](read_one.md) — hook\n"
              "- [Edited](edited.md) — a longer hook that costs more bytes\n"
              "- [Idle](idle.md) — never opened\n- [[old_one]] · [Idle](idle.md)\n")
        write(self.mem / "MEMORY_X.md", "- [Sub](sub.md) — second level\n")
        for f in ("read_one", "edited", "idle", "old_one", "sub", "unindexed"):
            write(self.mem / (f + ".md"), "x")
        write(self.mem / "archive" / "gone.md", "x")
        write(self.tdir / "s1.jsonl",
              use("Read", self.mem / "read_one.md", "2026-10-20T00:00:00Z")
              + use("Read", self.mem / "old_one.md", "2026-08-01T00:00:00Z")
              + use("Read", self.tmp.name + "/elsewhere.md", "2026-10-20T00:00:00Z"))
        write(self.tdir / "s1" / "subagents" / "a.jsonl",
              use("Edit", self.mem / "edited.md", "2026-10-21T00:00:00Z"))
        self.r = curate.curate(self.mem, [self.tdir], now="2026-10-25T00:00:00Z", days=30)

    def tearDown(self):
        self.tmp.cleanup()

    def row(self, name):
        return next(r for r in self.r["files"] if r["file"] == name)

    def test_reads_and_edits_count_subagents_included(self):
        self.assertEqual((self.row("read_one.md")["reads"], self.row("edited.md")["edits"]), (1, 1))

    def test_a_touch_before_the_window_does_not_count(self):
        self.assertEqual(self.row("old_one.md")["reads"], 0)

    def test_untouched_files_come_first_with_the_bytes_their_index_rows_cost(self):
        names = [r["file"] for r in self.r["files"]]
        self.assertEqual(set(names[:4]), {"idle.md", "old_one.md", "sub.md", "unindexed.md"})
        idle = self.row("idle.md")
        self.assertEqual(idle["indexes"], ["MEMORY.md"])
        self.assertEqual(idle["row_bytes"], len("- [Idle](idle.md) — never opened\n".encode())
                         + len("- [[old_one]] · [Idle](idle.md)\n".encode()))
        self.assertEqual(self.row("unindexed.md")["indexes"], [])

    def test_rows_naming_only_untouched_files_are_counted_once_by_index(self):
        auto = sum(len(l.encode()) for l in ["- [Idle](idle.md) — never opened\n",
                                              "- [[old_one]] · [Idle](idle.md)\n"])
        self.assertEqual(self.r["idle_rows"],
                         {"auto": auto, "second_level": len("- [Sub](sub.md) — second level\n".encode())})

    def test_indexes_and_archive_are_not_rows(self):
        names = {r["file"] for r in self.r["files"]}
        self.assertFalse({"MEMORY.md", "MEMORY_X.md", "archive/gone.md", "gone.md"} & names)

    def test_the_window_is_reported(self):
        w = self.r["window"]
        self.assertEqual((w["from"], w["sessions"]), ("2026-09-25T00:00:00Z", 1))

    def test_cli_prints_untouched_rows(self):
        env = dict(os.environ, QUORUM_MEMORY=str(self.mem), QUORUM_TRANSCRIPTS=str(self.tdir),
                   QUORUM_NOW="2026-10-25T00:00:00Z")
        p = subprocess.run([sys.executable, str(HERE / "quorum_cli.py"), "curate"], env=env,
                           capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("idle.md", p.stdout)
        self.assertNotIn("read_one.md", p.stdout)


if __name__ == "__main__":
    unittest.main()
