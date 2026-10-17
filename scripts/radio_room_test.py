#!/usr/bin/env python3
"""Tests for radio_room.py — the brainstorm room and its broadcast (QR-30…34):
resumable per-speaker sessions, the ported HANDOFF routing with the daemon's
guarantees, the editor pass and its checks. Fake models only."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import radio  # noqa: E402
import radio_room as room  # noqa: E402
from radio_test import CAST, LOCK, Fixture, fake_search  # noqa: E402

QUESTION = "Should the moderator gate run before the screen or after it?"


def handoff(to, prompt="What do you see?"):
    return "Thinking it over.\n\n```HANDOFF\nto: %s\nprompt: %s\n```" % (to, prompt)


class Scripted(room.FakeRoomModel):
    """The fake room with per-speaker scripted replies: {speaker: [reply, …]} (then the fake's own)."""

    def __init__(self, script=None):
        super().__init__()
        self.script = {k: list(v) for k, v in (script or {}).items()}

    def reply(self, speaker, session, system, user, view):
        q = self.script.get(speaker)
        if q:
            r = q.pop(0)
            return r(view) if callable(r) else r
        return super().reply(speaker, session, system, user, view)


class HandoffCase(unittest.TestCase):
    def test_the_old_block_parses(self):
        h = room.parse_handoff("I'd ask the Builder.\n\n```HANDOFF\nto: builder\nprompt: |\n  Read the moderator.\n  Then say.\n```")
        self.assertEqual(h, {"to": "builder", "prompt": "Read the moderator.\nThen say."})
        self.assertEqual(room.parse_handoff("```HANDOFF\nto: wrap\n```")["to"], "wrap")
        self.assertIsNone(room.parse_handoff("no block here"))

    def test_the_last_block_wins_and_a_bare_one_parses(self):
        t = "```HANDOFF\nto: builder\nprompt: a\n```\nthen\n```HANDOFF\nto: product\nprompt: b\n```"
        self.assertEqual(room.parse_handoff(t)["to"], "product")
        self.assertEqual(room.parse_handoff("HANDOFF\nto: verifier\nprompt: check it")["to"], "verifier")

    def test_verdict_and_threads(self):
        self.assertEqual(room.verdict_of("Long answer.\nVERDICT: the gate goes first."), "the gate goes first.")
        self.assertEqual(room.verdict_of("no verdict line at all, just words here"), "no verdict line at all, just words here")
        self.assertEqual(room.threads_of("Summary.\nTHREAD: one\nTHREAD: two\n"), ["one", "two"])


class CommandCase(unittest.TestCase):
    def test_a_first_turn_starts_a_session_and_a_later_one_resumes_it(self):
        m = room.RoomModel("/tmp/s", ["/r/a", "/v/f"], model="claude-opus-5-5")
        first = m.command({"id": "u-1", "started": False}, "SYS")
        later = m.command({"id": "u-1", "started": True}, "SYS")
        self.assertIn("--session-id", first)
        self.assertNotIn("-r", first)
        self.assertEqual(later[later.index("-r") + 1], "u-1")
        for c in (first, later):
            self.assertEqual(c[c.index("--tools") + 1], "Read,Grep,Glob")
            self.assertEqual(c[c.index("--model") + 1], "claude-opus-5-5")
            self.assertIn("--safe-mode", c)
            self.assertIn("--strict-mcp-config", c)
            self.assertEqual(c[c.index("--add-dir") + 1:c.index("--add-dir") + 3], ["/r/a", "/v/f"])
            self.assertIn("--max-budget-usd", c)
            self.assertEqual(c[c.index("--output-format") + 1], "json")


class RoomCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fx = Fixture(Path(self.tmp.name))
        self.cast = radio.load_cast(CAST, LOCK)
        self.inp = radio.gather_brainstorm("sidebar", radio.load_projects(self.fx.vault), self.fx.vault, self.fx.memory,
                                           QUESTION, [self.fx.home / "nonvis"], search=fake_search)

    def tearDown(self):
        self.tmp.cleanup()

    def run_room(self, script=None, chosen=("builder", "verifier", "architect", "product"), **kw):
        m = Scripted(script)
        return room.run_room(self.inp, self.cast, m, list(chosen), **kw), m

    def answers(self, r):
        return [t for t in r["turns"] if t["kind"] == "answer"]

    def test_everyone_chosen_speaks_and_the_host_closes_with_threads(self):
        r, m = self.run_room()
        self.assertEqual({t["speaker"] for t in self.answers(r)}, {"builder", "verifier", "architect", "product"})
        self.assertEqual(r["turns"][-1]["kind"], "close")
        self.assertTrue(r["threads"])
        self.assertLessEqual(r["calls"], room.ROOM_CALL_CAP)

    def test_each_speaker_keeps_one_session_started_once_then_resumed(self):
        r, m = self.run_room({"host": [handoff("builder"), handoff("product"), handoff("builder", "And the cost?")]})
        builder = [c for c in m.calls if c["speaker"] == "builder"]
        self.assertGreaterEqual(len(builder), 2)
        self.assertEqual(len({c["session"] for c in builder}), 1)
        self.assertEqual([c["started"] for c in builder][:2], [False, True])
        host = [c for c in m.calls if c["speaker"] == "host"]
        self.assertEqual(len({c["session"] for c in host}), 1)
        self.assertEqual(r["sessions"]["builder"], builder[0]["session"])

    def test_a_later_turn_carries_the_room_since_the_speaker_last_spoke(self):
        r, m = self.run_room({"host": [handoff("builder"), handoff("product"), handoff("builder", "Answer Product?")],
                              "product": ["Users would feel the delay.\nVERDICT: users notice."]})
        second = [c for c in m.calls if c["speaker"] == "builder"][1]
        self.assertIn("Users would feel the delay.", second["user"])
        self.assertIn("Answer Product?", second["user"])

    def test_the_first_turn_gets_the_brief_its_pointers_and_the_paths(self):
        r, m = self.run_room()
        first = next(c for c in m.calls if c["speaker"] == "architect")
        self.assertIn(QUESTION, first["user"])
        self.assertIn("58 - Radio Moves to Quorum", first["user"])          # its decision pointer
        self.assertIn(str(self.fx.home / "nonvis" / "sidebar"), first["user"])

    def test_an_unknown_to_goes_to_the_next_unasked(self):
        r, m = self.run_room({"host": [handoff("operator")]}, chosen=("builder", "product"))
        self.assertEqual(self.answers(r)[0]["speaker"], "builder")
        self.assertIn("not in the room", " ".join(r["log"]))

    def test_a_wrap_with_someone_unasked_asks_them_by_their_angle(self):
        r, m = self.run_room({"host": [handoff("builder"), handoff("wrap")]}, chosen=("builder", "product"))
        self.assertIn("product", [t["speaker"] for t in self.answers(r)])
        prod = next(c for c in m.calls if c["speaker"] == "product")
        self.assertIn("Would a user want it", prod["user"])

    def test_no_handoff_block_counts_as_the_next_unasked(self):
        r, m = self.run_room({"host": ["I have no block at all."]}, chosen=("builder", "product"))
        self.assertEqual({t["speaker"] for t in self.answers(r)}, {"builder", "product"})

    def test_two_passes_in_a_row_wrap(self):
        r, m = self.run_room({"builder": ["PASS"], "product": ["PASS"],
                              "host": [handoff("builder"), handoff("product"), handoff("builder")]},
                             chosen=("builder", "product"))
        self.assertIn("wrap: two passes in a row", r["log"])

    def test_the_cap_holds_and_the_close_is_reserved(self):
        loop = [handoff("builder", "More?")] * 40
        r, m = self.run_room({"host": loop}, chosen=("builder",), cap=6)
        self.assertLessEqual(r["calls"], 6)
        self.assertEqual(r["turns"][-1]["kind"], "close")

    def test_context_scripts_reach_the_brief(self):
        self.inp["context"] = [{"id": "x", "title": "Earlier", "question": "Before?", "transcript": "Builder: the gate exists."}]
        r, m = self.run_room()
        self.assertIn("Builder: the gate exists.", m.calls[0]["user"])

    def test_a_failed_first_call_raises_a_later_one_is_logged(self):
        class Down(Scripted):
            def reply(self, *a):
                raise radio.ModelError("down")
        with self.assertRaises(radio.ModelError):
            room.run_room(self.inp, self.cast, Down(), ["builder"])
        r, m = self.run_room({"product": [lambda v: (_ for _ in ()).throw(radio.ModelError("timeout"))]},
                             chosen=("builder", "product"))
        self.assertIn("failed", " ".join(r["log"]))

    def test_the_room_markdown_is_the_full_transcript(self):
        r, m = self.run_room()
        md = room.room_md(r, self.cast)
        self.assertIn(QUESTION, md)
        for t in self.answers(r):
            self.assertIn(t["text"].splitlines()[0], md)
        self.assertIn("## Open threads", md)


class EditorCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fx = Fixture(Path(self.tmp.name))
        self.cast = radio.load_cast(CAST, LOCK)
        inp = radio.gather_brainstorm("sidebar", radio.load_projects(self.fx.vault), self.fx.vault, self.fx.memory,
                                      QUESTION, [self.fx.home / "nonvis"], search=fake_search)
        self.room, _ = room.run_room(inp, self.cast, Scripted(), ["builder", "product"]), None

    def tearDown(self):
        self.tmp.cleanup()

    def edit(self, reply):
        class Ed(radio.FakeModel):
            def __call__(s, role, system, user, view):
                return json.dumps(reply) if not callable(reply) else reply(view)
        return room.broadcast(self.room, self.cast, Ed())

    def test_the_fake_editor_makes_a_whole_episode(self):
        ep = room.broadcast(self.room, self.cast, radio.FakeModel())
        on = [l for l in ep["lines"] if l["on_air"]]
        self.assertEqual((on[0]["speaker"], on[0]["kind"]), ("host", "open"))
        self.assertEqual((on[-1]["speaker"], on[-1]["kind"]), ("host", "signoff"))
        self.assertEqual({l["speaker"] for l in on} - {"host"}, {"builder", "product"})
        self.assertEqual(ep["kind"], "brainstorm")
        self.assertEqual(ep["room"]["question"], QUESTION)
        self.assertTrue(ep["threads"])

    def test_each_line_is_checked(self):
        turn = next(t["n"] for t in self.room["turns"] if t["speaker"] == "builder" and t["kind"] == "answer")
        ep = self.edit({"lines": [
            {"speaker": "host", "text": "Tonight, one question.", "turn": None},
            {"speaker": "builder", "text": "The gate is in the moderator (C2). " + "x " * 300, "turn": turn},
            {"speaker": "operator", "text": "I was never in the room.", "turn": turn},
            {"speaker": "product", "text": "A turn that does not exist.", "turn": 999},
            {"speaker": "host", "text": "Good night.", "turn": None}], "threads": ["one"]})
        on = [l for l in ep["lines"] if l["on_air"]]
        self.assertEqual([l["speaker"] for l in on], ["host", "builder", "host"])
        self.assertLessEqual(len(on[1]["text"]), radio.SHORT)
        self.assertNotIn("(C2)", on[1]["text"])
        self.assertEqual(on[1]["source"]["ref"], "room turn %d" % turn)
        self.assertIn("dropped", " ".join(ep["log"]))

    def test_a_missing_open_or_signoff_is_supplied(self):
        turn = next(t["n"] for t in self.room["turns"] if t["kind"] == "answer")
        ep = self.edit({"lines": [{"speaker": self.room["turns"][turn]["speaker"], "text": "One point.", "turn": turn}]})
        on = [l for l in ep["lines"] if l["on_air"]]
        self.assertEqual(on[0]["kind"], "open")
        self.assertEqual(on[-1]["kind"], "signoff")

    def test_an_editor_that_fails_still_airs_the_verdicts(self):
        ep = self.edit(lambda v: "not json")
        speakers = {l["speaker"] for l in ep["lines"] if l["on_air"]}
        self.assertEqual(speakers, {"host", "builder", "product"})
        self.assertIn("the editor's reply was not usable", " ".join(ep["log"]))


if __name__ == "__main__":
    unittest.main()
