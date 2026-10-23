#!/usr/bin/env python3
"""Tests for scripts/hook_replay.py — `quorum hook --replay`."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import hook_replay  # noqa: E402
import vault_index  # noqa: E402


def note(path, body, **front):
    path.parent.mkdir(parents=True, exist_ok=True)
    fm = ""
    if front:
        fm = "---\n" + "".join("%s: %s\n" % kv for kv in front.items()) + "---\n\n"
    path.write_text(fm + body, encoding="utf-8")


def prompt(ts, text):
    return json.dumps({"type": "user", "timestamp": ts,
                       "message": {"role": "user", "content": text}})


def read(ts, path):
    return json.dumps({"type": "assistant", "timestamp": ts, "message": {
        "content": [{"type": "tool_use", "name": "Read", "input": {"file_path": str(path)}}]}})


class ReplayCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.home = Path(self.tmp.name) / "home"
        self.vault = self.home / "vault"
        self.mem = self.home / "mem"
        self.mem.mkdir(parents=True)
        (self.mem / "MEMORY_PERMAFROST.md").write_text(
            "---\nname: pf\ndescription: Permafrost's memory index\n---\n\nhooks\n")
        pf = self.vault / "01_Projects" / "Permafrost"
        note(pf / "28 - Briefing.md", "where it is", title="Briefing")
        note(pf / "07 - Litestream Restore.md", "litestream restores the WAL",
             title="Litestream Restore")
        note(self.vault / "Action Queue" / "94 - Asks.md", "asks", title="Asks")
        (self.vault / "06_Metadata").mkdir()
        (self.vault / "06_Metadata" / "quorum-projects.json").write_text(json.dumps({
            "v": 1, "projects": {"permafrost": {
                "name": "Permafrost", "folders": ["01_Projects/Permafrost"],
                "notes": ["01_Projects/Permafrost/28 - *.md"]}}}))
        # Every note enters the vault's history on 2026-10-01.
        when = dict(os.environ, GIT_AUTHOR_DATE="2026-10-01T00:00:00Z",
                    GIT_COMMITTER_DATE="2026-10-01T00:00:00Z")
        for cmd in (["init", "-q"], ["add", "-A"],
                    ["-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "notes"]):
            subprocess.run(["git", "-C", str(self.vault)] + cmd, check=True, env=when)
        munged = "".join("-" if c in "/." else c for c in str(self.vault))
        self.tdir = self.home / ".claude" / "projects" / munged
        self.tdir.mkdir(parents=True)
        self.lit = pf / "07 - Litestream Restore.md"
        # These prompts come after the notes' first commit.
        (self.tdir / "s1.jsonl").write_text("\n".join([
            prompt("2099-01-01T00:00:00Z", "let's work on permafrost"),
            read("2099-01-01T00:00:05Z", self.mem / "MEMORY_PERMAFROST.md"),
            prompt("2099-01-01T00:01:00Z", "permafrost again"),
            json.dumps({"type": "user", "timestamp": "2099-01-01T00:01:30Z", "toolUseResult": {},
                        "message": {"content": [{"type": "tool_result", "content": "x"}]}}),
            prompt("2099-01-01T00:01:40Z", "<task-notification>permafrost/07</task-notification>"),
            prompt("2099-01-01T00:02:00Z", "push"),
            read("2099-01-01T00:02:09Z", self.lit),
            read("2099-01-01T00:02:10Z", self.home / "elsewhere.md"),
        ]) + "\n")
        # A prompt older than every note's first commit: nothing it could have
        # been shown or needed, though each file was born (written) after it.
        (self.tdir / "s0.jsonl").write_text("\n".join([
            prompt("2000-01-01T00:00:00Z", "see AQ/94"),
            read("2000-01-01T00:00:05Z", self.vault / "Action Queue" / "94 - Asks.md"),
        ]) + "\n")

    def tearDown(self):
        self.tmp.cleanup()

    def run_replay(self):
        idx = vault_index.Index(self.vault / ".quorum" / "index.db", self.vault, self.mem)
        idx.refresh()
        try:
            return hook_replay.replay(idx, self.tdir)
        finally:
            idx.close()

    def test_scores_each_prompt_against_the_notes_its_turn_read(self):
        s = self.run_replay()["summary"]
        self.assertEqual((s["sessions"], s["prompts"], s["needing"]), (2, 4, 2))
        self.assertEqual((s["fired"], s["useful"], s["hits"], s["hits_right"]), (1, 1, 2, 1))
        self.assertEqual((s["precision"], s["coverage"]), (0.5, 0.5))

    def test_a_note_already_shown_in_the_session_is_not_shown_again(self):
        t = [t for t in self.run_replay()["turns"] if t["prompt"] == "permafrost again"][0]
        self.assertEqual(t["hits"], [])

    def test_harness_messages_and_tool_results_are_no_prompts(self):
        texts = [t["prompt"] for t in self.run_replay()["turns"]]
        self.assertFalse(any(t.startswith("<task-notification") for t in texts))
        self.assertEqual(len(texts), 4)

    def test_a_note_committed_after_the_prompt_is_neither_hit_nor_answer(self):
        t0 = [t for t in self.run_replay()["turns"] if t["session"] == "s0"][0]
        self.assertEqual((t0["hits"], t0["needed"]), ([], []))

    def test_a_note_rewritten_after_the_prompt_still_counts_from_its_first_commit(self):
        (self.tdir / "s2.jsonl").write_text("\n".join([
            prompt("2026-10-10T00:00:00Z", "see permafrost/07"),
            read("2026-10-10T00:00:05Z", self.lit),
        ]) + "\n")
        t2 = [t for t in self.run_replay()["turns"] if t["session"] == "s2"][0]
        self.assertEqual(t2["right"], ["vault:01_Projects/Permafrost/07 - Litestream Restore.md"])

    def test_a_note_read_earlier_in_the_session_is_not_needed_again(self):
        # A re-read (before an Edit, after a compaction) is a note the session
        # already has: no injection could have saved it.
        brief = self.vault / "01_Projects" / "Permafrost" / "28 - Briefing.md"
        (self.tdir / "s3.jsonl").write_text("\n".join([
            prompt("2099-01-02T00:00:00Z", "let's work on permafrost"),
            read("2099-01-02T00:00:05Z", brief),
            prompt("2099-01-02T00:01:00Z", "update the briefing"),
            read("2099-01-02T00:01:05Z", brief),
            read("2099-01-02T00:01:06Z", self.lit),
        ]) + "\n")
        t = [t for t in self.run_replay()["turns"] if t["session"] == "s3"]
        self.assertEqual(t[0]["needed"], ["vault:01_Projects/Permafrost/28 - Briefing.md"])
        self.assertEqual(t[1]["needed"], ["vault:01_Projects/Permafrost/07 - Litestream Restore.md"])

    def test_a_read_record_without_a_timestamp_is_no_earlier_read(self):
        # Transcripts carry untimed copies of tool calls; "" sorts before every prompt.
        brief = self.vault / "01_Projects" / "Permafrost" / "28 - Briefing.md"
        untimed = json.loads(read("x", brief))
        del untimed["timestamp"]
        (self.tdir / "s4.jsonl").write_text("\n".join([
            prompt("2099-01-03T00:00:00Z", "let's work on permafrost"),
            read("2099-01-03T00:00:05Z", brief),
            json.dumps(untimed),
        ]) + "\n")
        t = [t for t in self.run_replay()["turns"] if t["session"] == "s4"][0]
        self.assertIn("vault:01_Projects/Permafrost/28 - Briefing.md", t["right"])

    def test_cli_replay_json(self):
        env = dict(os.environ, HOME=str(self.home), QUORUM_VAULT=str(self.vault),
                   QUORUM_MEMORY=str(self.mem))
        p = subprocess.run([sys.executable, str(HERE / "quorum_cli.py"), "hook", "--replay",
                            "--json"], env=env, capture_output=True, text=True,
                           stdin=subprocess.DEVNULL)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(json.loads(p.stdout)["summary"]["useful"], 1)


if __name__ == "__main__":
    unittest.main()
