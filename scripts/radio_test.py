#!/usr/bin/env python3
"""Tests for scripts/radio.py — `quorum radio`: the cast file and its voice lock,
the $0 inputs, the host-run text round (on a fake model), the episode note and
the voice job handed to the sidecar. Run: python3 scripts/radio_test.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))

import radio  # noqa: E402

CAST = REPO / "radio" / "cast.json"
LOCK = REPO / "radio" / "voices.lock"


def git(cwd, *args, env=None):
    e = dict(os.environ)
    e.update({"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t",
              "GIT_COMMITTER_EMAIL": "t@t"})
    e.update(env or {})
    return subprocess.run(["git", "-C", str(cwd)] + list(args), env=e, check=True,
                          capture_output=True, text=True).stdout


class CastCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, cast, lock=None):
        c = self.dir / "cast.json"
        c.write_text(json.dumps({"v": 1, "cast": cast}))
        lk = self.dir / "voices.lock"
        lk.write_text(json.dumps({"v": 1, "voices": lock or {}}))
        return c, lk

    def test_the_tracked_cast_is_the_designs_table(self):
        cast = radio.load_cast(CAST, LOCK)
        got = {d["id"]: (d["name"], d["voice"], d["pan"]) for d in cast}
        self.assertEqual(got, {
            "host": ("Host", "af_heart", "centre"),
            "meridian": ("Meridian", "am_michael", "far left"),
            "permafrost": ("Permafrost", "bf_emma", "far right"),
            "sidebar": ("Sidebar", "af_nicole", "left"),
            "bastion": ("Bastion", "bm_george", "right"),
            "skeptic": ("Skeptic", "am_fenrir", "centre-left"),
            "accountant": ("Accountant", "am_puck", "centre-right"),
        })
        for d in cast:
            self.assertEqual(set(d) - {"pan_value"}, {"id", "name", "voice", "pan", "reads"})
        self.assertEqual(json.loads(LOCK.read_text())["voices"],
                         {d["id"]: d["voice"] for d in cast})
        # Bastion's repos are off the allowlist (#99): the desk reads no repo.
        self.assertEqual([d for d in cast if d["id"] == "bastion"][0]["reads"]["repos"], [])

    def test_a_duplicate_voice_is_refused(self):
        c, lk = self.write([
            {"id": "a", "name": "A", "voice": "am_michael", "pan": "left", "reads": {}},
            {"id": "b", "name": "B", "voice": "am_michael", "pan": "right", "reads": {}}])
        with self.assertRaisesRegex(radio.CastError, "am_michael"):
            radio.load_cast(c, lk)

    def test_a_voice_outside_the_english_presets_is_refused(self):
        for v in ("ff_siwis", "jf_alpha", "am_nobody", "zf_xiaobei"):
            c, lk = self.write([{"id": "a", "name": "A", "voice": v, "pan": "left", "reads": {}}])
            with self.assertRaisesRegex(radio.CastError, "preset"):
                radio.load_cast(c, lk)

    def test_an_unknown_pan_is_refused(self):
        c, lk = self.write([{"id": "a", "name": "A", "voice": "am_puck", "pan": "upstairs", "reads": {}}])
        with self.assertRaisesRegex(radio.CastError, "pan"):
            radio.load_cast(c, lk)

    def test_a_changed_voice_for_a_locked_id_is_refused(self):
        c, lk = self.write([{"id": "a", "name": "A", "voice": "am_puck", "pan": "left", "reads": {}}],
                           {"a": "am_michael"})
        with self.assertRaisesRegex(radio.CastError, "never reassigned"):
            radio.load_cast(c, lk)

    def test_a_retired_ids_voice_stays_reserved(self):
        c, lk = self.write([{"id": "new", "name": "N", "voice": "am_michael", "pan": "left", "reads": {}}],
                           {"old": "am_michael"})
        with self.assertRaisesRegex(radio.CastError, "reserved"):
            radio.load_cast(c, lk)

    def test_record_appends_a_new_id_and_never_rewrites_an_old_one(self):
        c, lk = self.write([{"id": "a", "name": "A", "voice": "am_puck", "pan": "left", "reads": {}},
                            {"id": "b", "name": "B", "voice": "bf_emma", "pan": "right", "reads": {}}],
                           {"a": "am_puck", "gone": "am_echo"})
        radio.load_cast(c, lk, record=True)
        self.assertEqual(json.loads(lk.read_text())["voices"],
                         {"a": "am_puck", "gone": "am_echo", "b": "bf_emma"})

    def test_pans_map_to_numbers_left_negative(self):
        cast = radio.load_cast(CAST, LOCK)
        p = {d["id"]: d["pan_value"] for d in cast}
        self.assertEqual(p["host"], 0.0)
        self.assertLess(p["meridian"], p["sidebar"])
        self.assertLess(p["sidebar"], p["skeptic"])
        self.assertLess(p["skeptic"], 0)
        self.assertGreater(p["accountant"], 0)
        self.assertLess(p["accountant"], p["bastion"])
        self.assertLess(p["bastion"], p["permafrost"])


NOW = datetime(2026, 10, 17, 2, 0, tzinfo=timezone.utc)


class Fixture:
    """A temp HOME with a vault, a memory dir, two git repos and the tracked cast."""

    def __init__(self, root):
        self.root = root
        self.home = root / "home"
        self.vault = self.home / "vault"
        self.memory = root / "memory"
        for p in (self.vault / "06_Metadata", self.vault / "Action Queue", self.memory,
                  self.home / "nonvis"):
            p.mkdir(parents=True)
        (self.vault / "06_Metadata" / "quorum-repos.txt").write_text(
            "# allowlist\nmeridian\nsidebar\npermafrost\n")
        recent = "2026-10-16T20:00:00Z"
        old = "2026-10-10T00:00:00Z"
        self.note("01_Projects/Meridian/08 - Briefing — Where Meridian Is (rolling).md",
                  "# M\n\n## 1 · State\nOps/139 fixed the TFS rewrite.\n\n## 3 · Owed\n- ⚖ control token on GCP\n- linger on the MSI\n\n"
                  "spend: `quorum spend --session aaaa1111` = main $1.00 · total $3.50 EST; window 2026-10-16T20:00Z → 23:00Z\n"
                  "spend: `quorum spend --session bbbb2222` = main $9.00 · total $9.00 EST; window 2026-10-01T01:00Z → 02:00Z\n",
                  mtime=NOW - timedelta(hours=2))
        self.note("01_Projects/Sidebar/13 - Briefing — Where Sidebar Is (rolling).md",
                  "# S\n\n## §1 State\nRadio 4th mode.\n\n## §3 Owed\n- listen\n", mtime=NOW - timedelta(days=3))
        self.note("01_Projects/Permafrost/28 - Briefing — Where Permafrost Is (rolling).md",
                  "# P\n\n## 1 · State\nold\n\n## 3 · Owed\n- nothing\n", mtime=NOW - timedelta(days=3))
        self.note("01_Projects/Loqua/MVP/00 - Index & Status.md", "# L\n", mtime=NOW - timedelta(days=3))
        self.note("01_Projects/Partners/Samsung/Bastion/21 - Briefing — Where Bastion Is (rolling).md",
                  "# B\n\n## 1. State\n#119 merged.\n\n## 3. Owed\n- BAS-242\n\n"
                  "- **10-16 session `cccc3333`:** spend: `quorum spend --session cccc3333` = total $2.00 EST; window 2026-10-16T21:00Z → 22:00Z\n", mtime=NOW - timedelta(hours=5))
        self.note("01_Projects/Partners/Samsung/Linear/00 - Ticket Ledger (rolling).md", "# T\n",
                  mtime=NOW - timedelta(days=3))
        (self.vault / "Action Queue" / "15 - Meridian — parked rulings (2026-09-01).md").write_text("x")
        (self.vault / "Action Queue" / "20 - GCP migration (2026-09-04).md").write_text("x")
        (self.memory / "MEMORY_VERIFICATION.md").write_text(
            "# V\n\n- [☠☠☠ TESTED ≠ CALLED](x.md) — 16 tests, 0 call sites\n- [☠ minor](y.md) — small\n")
        (self.memory / "MEMORY_VERIFICATION_2.md").write_text(
            "- [☠☠☠ A MODEL of the quantity is not the quantity](z.md) — read or believe\n")
        # Repos: meridian has a recent and an old commit; sidebar only an old one; a repo
        # off the allowlist (loqua-passkey) has a recent commit that must not be read.
        self.repo("meridian", [("old meridian work", old), ("ops/139: TFS rewrite", recent)])
        self.repo("sidebar", [("old sidebar work", old)])
        self.repo("loqua-passkey", [("secret loqua work", recent)])

    def note(self, rel, text, mtime):
        p = self.vault / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        t = mtime.timestamp()
        os.utime(p, (t, t))

    def repo(self, name, commits):
        r = self.home / "nonvis" / name
        r.mkdir()
        git(r, "init", "-q", "-b", "main")
        for i, (subject, when) in enumerate(commits):
            (r / ("f%d" % i)).write_text(subject)
            git(r, "add", ".")
            git(r, "commit", "-q", "-m", subject,
                env={"GIT_AUTHOR_DATE": when, "GIT_COMMITTER_DATE": when})

    def env(self):
        return {"HOME": str(self.home), "QUORUM_VAULT": str(self.vault),
                "QUORUM_MEMORY": str(self.memory)}


class InputsCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fx = Fixture(Path(self.tmp.name))
        self.cast = radio.load_cast(CAST, LOCK)

    def tearDown(self):
        self.tmp.cleanup()

    def inputs(self):
        return radio.gather_inputs(self.cast, self.fx.vault, self.fx.memory,
                                   since=NOW - timedelta(hours=24), until=NOW,
                                   repo_roots=[self.fx.home / "nonvis"])

    def desk(self, inp, i):
        return [d for d in inp["desks"] if d["id"] == i][0]

    def test_an_active_desk_reads_its_briefing_commits_spend_and_action_queue(self):
        m = self.desk(self.inputs(), "meridian")
        self.assertTrue(m["on_air"])
        self.assertEqual([n["path"] for n in m["notes"]],
                         ["01_Projects/Meridian/08 - Briefing — Where Meridian Is (rolling).md"])
        self.assertEqual([c["subject"] for c in m["commits"]], ["ops/139: TFS rewrite"])
        self.assertEqual(len(m["spend"]), 1)
        self.assertIn("aaaa1111", m["spend"][0]["line"])
        self.assertEqual([a["title"] for a in m["action_queue"]],
                         ["15 - Meridian — parked rulings (2026-09-01)"])

    def test_a_desk_with_no_activity_in_the_window_is_off_the_cast(self):
        inp = self.inputs()
        self.assertFalse(self.desk(inp, "sidebar")["on_air"])
        self.assertFalse(self.desk(inp, "permafrost")["on_air"])
        self.assertIn("no activity", self.desk(inp, "sidebar")["why"])

    def test_a_repo_off_the_allowlist_is_never_read(self):
        p = self.desk(self.inputs(), "permafrost")
        self.assertEqual(p["commits"], [])
        self.assertIn("loqua-passkey", p["skipped_repos"])
        self.assertNotIn("secret loqua work", json.dumps(self.inputs()))

    def test_bastion_is_on_air_from_its_note_alone(self):
        b = self.desk(self.inputs(), "bastion")
        self.assertTrue(b["on_air"])
        self.assertEqual(b["commits"], [])

    def test_the_skeptic_and_the_accountant_always_sit(self):
        fx = self.fx
        inp = radio.gather_inputs(self.cast, fx.vault, fx.memory,
                                  since=NOW + timedelta(days=30), until=NOW + timedelta(days=31),
                                  repo_roots=[fx.home / "nonvis"])
        on = [d["id"] for d in inp["desks"] if d["on_air"]]
        self.assertEqual(on, ["host", "skeptic", "accountant"])

    def test_the_skeptic_reads_the_top_verification_rules(self):
        s = self.desk(self.inputs(), "skeptic")
        texts = " ".join(r["text"] for r in s["rules"])
        self.assertIn("TESTED ≠ CALLED", texts)
        self.assertIn("A MODEL of the quantity", texts)
        self.assertNotIn("minor", texts)

    def test_the_accountant_reads_every_window_spend_line_and_the_owed_rows(self):
        a = self.desk(self.inputs(), "accountant")
        self.assertEqual([s["line"][:30] for s in a["spend"]],
                         ["spend: `quorum spend --session"] * 2)
        self.assertIn("cccc3333", a["spend"][1]["line"])
        owed = " ".join(r for o in a["owed"] for r in o["rows"])
        self.assertIn("control token on GCP", owed)
        self.assertIn("BAS-242", owed)

    def test_a_bulleted_spend_line_counts(self):
        b = self.desk(self.inputs(), "bastion")
        self.assertEqual([s["line"][:7] for s in b["spend"]], ["spend: "])

    def test_the_host_reads_nothing_and_is_always_on(self):
        h = self.desk(self.inputs(), "host")
        self.assertTrue(h["on_air"])
        self.assertEqual(h["notes"], [])

    def test_spend_window_end_may_be_time_only_or_a_full_stamp(self):
        self.assertEqual(radio.spend_window_end(
            "spend: x; window 2026-10-15T02:24Z → 09:07Z"),
            datetime(2026, 10, 15, 9, 7, tzinfo=timezone.utc))
        self.assertEqual(radio.spend_window_end(
            "spend: x; window 2026-10-15T09:32Z → 2026-10-16T10:25Z"),
            datetime(2026, 10, 16, 10, 25, tzinfo=timezone.utc))
        # An end clock before the start's means the window crossed midnight.
        self.assertEqual(radio.spend_window_end(
            "spend: x; window 2026-10-15T23:30Z → 01:10Z"),
            datetime(2026, 10, 16, 1, 10, tzinfo=timezone.utc))
        self.assertIsNone(radio.spend_window_end("spend: no window"))


class ScriptedModel:
    """Replies from a list, in order; records every call (role, view)."""

    def __init__(self, replies):
        self.replies = list(replies)
        self.calls = []

    def __call__(self, role, system, user, view):
        self.calls.append((role, view))
        if not self.replies:
            return json.dumps({"action": "signoff", "say": "Good night."})
        r = self.replies.pop(0)
        return r(role, view) if callable(r) else r


class RoundCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fx = Fixture(Path(self.tmp.name))
        self.cast = radio.load_cast(CAST, LOCK)
        self.inp = radio.gather_inputs(self.cast, self.fx.vault, self.fx.memory,
                                       since=NOW - timedelta(hours=24), until=NOW,
                                       repo_roots=[self.fx.home / "nonvis"])

    def tearDown(self):
        self.tmp.cleanup()

    def run_fake(self, **kw):
        return radio.run_episode(self.inp, self.cast, radio.FakeModel(), topic="daily night recap", **kw)

    def test_the_fake_round_stays_under_the_cap_and_ends_with_the_host_sign_off(self):
        ep = self.run_fake()
        self.assertLessEqual(ep["calls"]["total"], 14)
        on = [l for l in ep["lines"] if l["on_air"]]
        self.assertEqual(on[0]["speaker"], "host")
        self.assertEqual(on[-1]["speaker"], "host")
        self.assertEqual(on[-1]["kind"], "signoff")
        self.assertEqual(ep["cast"], ["host", "meridian", "bastion", "skeptic", "accountant"])

    def test_every_desk_line_on_air_has_a_source_or_is_a_pass(self):
        ep = self.run_fake()
        desk_lines = [l for l in ep["lines"] if l["on_air"] and l["speaker"] != "host"]
        self.assertTrue(desk_lines)
        for l in desk_lines:
            if l["kind"] == "pass":
                self.assertEqual(l["text"], "No opinion here.")
            else:
                self.assertTrue(l["source"], l)

    def test_the_host_names_the_desk_it_brings_in(self):
        ep = self.run_fake()
        lines = ep["lines"]
        for i, l in enumerate(lines):
            if l["on_air"] and l["kind"] == "pick":
                nxt = lines[i + 1]
                self.assertIn(radio.spoken_name(nxt["speaker"]).replace("the ", "").lower(),
                              l["text"].lower())

    def test_each_pick_logs_a_one_line_reason(self):
        ep = self.run_fake()
        picks = [l for l in ep["lines"] if l["kind"] == "answer" and l["on_air"]]
        self.assertTrue(picks)
        for l in picks:
            self.assertTrue(l["reason"])
            self.assertNotIn("\n", l["reason"])

    def test_spoken_labels(self):
        self.assertTrue(radio.with_label("The rewrite landed.", "grounded").startswith("I know"))
        self.assertTrue(radio.with_label("It slips a week.", "inferred").lower().startswith("my guess"))
        self.assertTrue(radio.with_label("It was tested, not called.", "borrowed", "meridian")
                        .startswith("The Meridian desk says"))
        self.assertEqual(radio.with_label("I know the rewrite landed.", "grounded"),
                         "I know the rewrite landed.")

    def test_an_answer_without_a_resolvable_source_is_a_pass(self):
        host_open = json.dumps({"action": "ask", "say": "Good evening. What moved?",
                                "ask": {"to": "cast", "question": "What moved?"}})
        bad = json.dumps({"answer": "Things happened.", "label": "grounded", "source": "N99"})
        nosrc = json.dumps({"answer": "Things happened.", "label": "grounded"})
        model = ScriptedModel([host_open, bad, nosrc, json.dumps({"pass": True}), bad])
        ep = radio.run_episode(self.inp, self.cast, model, topic="t")
        self.assertFalse([l for l in ep["lines"] if l["kind"] == "answer"])
        # Nothing to pick and the question drew < 2 answers: the next host call is the sign-off.
        self.assertEqual(ep["lines"][-1]["kind"], "signoff")
        self.assertEqual(ep["calls"]["total"], 6)

    def test_gr8_no_new_question_when_the_last_cast_question_drew_fewer_than_two(self):
        host_open = json.dumps({"action": "ask", "say": "What moved?",
                                "ask": {"to": "cast", "question": "What moved?"}})
        one = json.dumps({"answer": "The TFS rewrite merged.", "label": "grounded", "source": "C1"})
        pick = json.dumps({"action": "pick", "desk": "meridian", "say": "Meridian, what landed?",
                           "reason": "the only answer"})
        ask_again = json.dumps({"action": "ask", "say": "Bastion?", "ask": {"to": "bastion", "question": "Anything?"}})
        model = ScriptedModel([host_open, one, json.dumps({"pass": True}), json.dumps({"pass": True}),
                               json.dumps({"pass": True}), pick, ask_again])
        ep = radio.run_episode(self.inp, self.cast, model, topic="t")
        self.assertFalse([l for l in ep["lines"] if l["kind"] in ("ask",) and l["on_air"]])
        self.assertEqual(ep["lines"][-1]["kind"], "signoff")
        self.assertIn("GR-8", " ".join(ep["log"]))

    def test_the_cap_holds_and_the_last_call_is_the_sign_off(self):
        def host(role, view):
            if role != "host":
                return json.dumps({"answer": "Line %d." % len(view.get("on_air", [])),
                                   "label": "grounded", "source": view["sources"][0]["id"]})
            if view["mode"] == "signoff":
                return json.dumps({"action": "signoff", "say": "Good night."})
            if view["may_ask"]:
                return json.dumps({"action": "ask", "say": "Next?", "ask": {"to": "cast", "question": "Next?"}})
            return json.dumps({"action": "pick", "desk": view["answers"][0]["desk"], "say": "Go.", "reason": "r"})
        model = ScriptedModel([host] * 100)
        ep = radio.run_episode(self.inp, self.cast, model, topic="t")
        self.assertLessEqual(ep["calls"]["total"], 14)
        self.assertEqual(model.calls[-1][0], "host")
        self.assertEqual(model.calls[-1][1]["mode"], "signoff")

    def test_a_repeat_back_is_confirmed_by_the_desk_in_the_next_call(self):
        host_open = json.dumps({"action": "ask", "say": "What moved?",
                                "ask": {"to": "cast", "question": "What moved?"}})
        m = json.dumps({"answer": "The TFS rewrite merged.", "label": "grounded", "source": "C1"})
        b = json.dumps({"answer": "PR 119 merged.", "label": "grounded", "source": "N1"})
        pick = json.dumps({"action": "pick", "desk": "meridian", "say": "Meridian, you first.", "reason": "fresh code"})
        rb = json.dumps({"action": "pick", "repeat_back": "the TFS rewrite merged", "desk": "bastion",
                         "say": "Bastion?", "reason": "second answer"})
        confirm = json.dumps({"verdict": "confirm", "say": "Yes, merged on main."})
        model = ScriptedModel([host_open, m, b, json.dumps({"pass": True}), json.dumps({"pass": True}),
                               pick, rb, confirm])
        ep = radio.run_episode(self.inp, self.cast, model, topic="t")
        kinds = [(l["speaker"], l["kind"]) for l in ep["lines"] if l["on_air"]]
        i = kinds.index(("host", "repeat"))
        self.assertEqual(kinds[i + 1], ("meridian", "confirm"))
        rep = [l for l in ep["lines"] if l["kind"] == "repeat"][0]
        self.assertTrue(rep["text"].startswith("So, Meridian, you're saying"))

    def test_a_withdrawal_takes_the_claim_off_air(self):
        host_open = json.dumps({"action": "ask", "say": "What moved?",
                                "ask": {"to": "cast", "question": "What moved?"}})
        m = json.dumps({"answer": "The TFS rewrite merged.", "label": "grounded", "source": "C1"})
        b = json.dumps({"answer": "PR 119 merged.", "label": "grounded", "source": "N1"})
        pick = json.dumps({"action": "pick", "desk": "meridian", "say": "Meridian?", "reason": "r"})
        rb = json.dumps({"action": "pick", "repeat_back": "the TFS rewrite merged", "desk": "bastion",
                         "say": "Bastion?", "reason": "r"})
        model = ScriptedModel([host_open, m, b, json.dumps({"pass": True}), json.dumps({"pass": True}),
                               pick, rb, json.dumps({"verdict": "withdraw", "say": "Not sure, actually."})])
        ep = radio.run_episode(self.inp, self.cast, model, topic="t")
        merid = [l for l in ep["lines"] if l["speaker"] == "meridian"]
        self.assertTrue(merid)
        self.assertFalse([l for l in merid if l["on_air"]])

    def test_a_desk_asked_directly_with_nothing_says_no_opinion_here(self):
        host_open = json.dumps({"action": "ask", "say": "What moved?",
                                "ask": {"to": "cast", "question": "What moved?"}})
        m = json.dumps({"answer": "The TFS rewrite merged.", "label": "grounded", "source": "C1"})
        b = json.dumps({"answer": "PR 119 merged.", "label": "grounded", "source": "N1"})
        ask_sk = json.dumps({"action": "ask", "say": "Skeptic, anything to doubt?",
                             "ask": {"to": "skeptic", "question": "Anything to doubt?"}, "reason": "challenge"})
        model = ScriptedModel([host_open, m, b, json.dumps({"pass": True}), json.dumps({"pass": True}),
                               ask_sk, json.dumps({"pass": True})])
        ep = radio.run_episode(self.inp, self.cast, model, topic="t")
        sk = [l for l in ep["lines"] if l["speaker"] == "skeptic"]
        self.assertEqual([(l["kind"], l["text"]) for l in sk], [("pass", "No opinion here.")])

    def test_the_skeptic_may_cite_another_desks_line_as_borrowed(self):
        host_open = json.dumps({"action": "ask", "say": "What moved?",
                                "ask": {"to": "cast", "question": "What moved?"}})
        m = json.dumps({"answer": "The TFS rewrite merged.", "label": "grounded", "source": "C1"})
        b = json.dumps({"answer": "PR 119 merged.", "label": "grounded", "source": "N1"})
        pick = json.dumps({"action": "pick", "desk": "meridian", "say": "Meridian?", "reason": "r"})
        ask_sk = json.dumps({"action": "ask", "say": "Skeptic, does that hold?",
                             "ask": {"to": "skeptic", "question": "Does that hold?"}})
        def sk(role, view):
            line = [s for s in view["sources"] if s["id"].startswith("L")][0]
            return json.dumps({"answer": "merged is not the same as running.", "label": "borrowed",
                               "source": line["id"]})
        model = ScriptedModel([host_open, m, b, json.dumps({"pass": True}), json.dumps({"pass": True}),
                               pick, ask_sk, sk])
        ep = radio.run_episode(self.inp, self.cast, model, topic="t")
        s = [l for l in ep["lines"] if l["speaker"] == "skeptic" and l["on_air"]][0]
        self.assertEqual(s["label"], "borrowed")
        self.assertIn("The Meridian desk says merged", s["text"])

    def test_the_first_failed_call_aborts_the_episode(self):
        def boom(role, system, user, view):
            raise radio.ModelError("claude: unknown option")
        with self.assertRaises(radio.ModelError):
            radio.run_episode(self.inp, self.cast, boom, topic="t")


class NoteAndVoiceCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fx = Fixture(Path(self.tmp.name))
        self.cast = radio.load_cast(CAST, LOCK)
        self.inp = radio.gather_inputs(self.cast, self.fx.vault, self.fx.memory,
                                       since=NOW - timedelta(hours=24), until=NOW,
                                       repo_roots=[self.fx.home / "nonvis"])
        self.ep = radio.run_episode(self.inp, self.cast, radio.FakeModel(), topic="daily night recap")

    def tearDown(self):
        self.tmp.cleanup()

    def test_the_note_is_numbered_in_its_folder_and_carries_the_transcript(self):
        d = self.fx.vault / "01_Projects" / "Quorum" / "Radio"
        p0 = radio.write_note(self.ep, self.cast, self.fx.vault, day="2026-10-17")
        self.assertEqual(p0, d / "00 - Night Recap (2026-10-17).md")
        p1 = radio.write_note(self.ep, self.cast, self.fx.vault, day="2026-10-17")
        self.assertEqual(p1.name, "01 - Night Recap (2026-10-17).md")
        t = p0.read_text()
        fm = t.split("---")[1]
        for k in ("title:", "date: 2026-10-17", "tags:", "summary:"):
            self.assertIn(k, fm)
        self.assertIn("calls: %d of 14" % self.ep["calls"]["total"], t)
        for l in self.ep["lines"]:
            if l["on_air"]:
                self.assertIn(radio.voice_of(self.cast, l["speaker"]), t)
        self.assertIn("am_michael", t)
        self.assertIn("ops/139: TFS rewrite", t)  # a commit source, cited

    def test_a_topic_titles_the_note(self):
        p = radio.write_note(self.ep, self.cast, self.fx.vault, day="2026-10-17", topic="the bastion week")
        self.assertEqual(p.name, "00 - The Bastion Week (2026-10-17).md")

    def test_the_voice_job_binds_each_line_to_its_speakers_voice_and_pan(self):
        job = radio.voice_job(self.ep, self.cast)
        on = [l for l in self.ep["lines"] if l["on_air"]]
        self.assertEqual(len(job["lines"]), len(on))
        by = {d["id"]: d for d in self.cast}
        for j, l in zip(job["lines"], on):
            self.assertEqual(j["voice"], by[l["speaker"]]["voice"])
            self.assertEqual(j["pan"], by[l["speaker"]]["pan_value"])
            self.assertGreater(j["pause"], 0)
        self.assertEqual(job["voices"], sorted({d["voice"] for d in self.cast}))

    def test_the_voice_job_speaks_no_symbols(self):
        ep = {"lines": [{"i": 0, "speaker": "host", "kind": "open", "text": "Good ☠☠☠ evening ⭐ ✅ — all.",
                         "on_air": True}]}
        job = radio.voice_job(ep, self.cast)
        self.assertEqual(job["lines"][0]["text"], "Good evening — all.")

    def test_pauses_come_from_structure(self):
        P = radio.pause_before
        q = {"speaker": "host", "kind": "ask", "text": "What moved?"}
        a = {"speaker": "meridian", "kind": "answer", "text": "x"}
        s = {"speaker": "host", "kind": "signoff", "text": "Good night."}
        self.assertGreater(P(q, a), P(a, {"speaker": "bastion", "kind": "answer", "text": "y"}))
        self.assertGreater(P(a, s), P(q, a))
        self.assertEqual(P(None, q), radio.PAUSES["lead"] * radio.NIGHT_STRETCH)

    @unittest.skipUnless(shutil.which("node") and shutil.which("ffmpeg") and shutil.which("ffprobe"),
                         "node + ffmpeg needed")
    def test_the_sidecar_renders_one_mp3_with_a_fake_tts(self):
        out = Path(self.tmp.name) / "ep.mp3"
        r = radio.render(self.ep, self.cast, out, fake_tts=True)
        self.assertTrue(out.exists())
        self.assertGreater(r["duration"], 1.0)
        dur = float(subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration",
                                    "-of", "csv=p=0", str(out)], capture_output=True, text=True).stdout)
        self.assertAlmostEqual(dur, r["duration"], delta=0.5)
        self.assertEqual(len(r["lines"]), len([l for l in self.ep["lines"] if l["on_air"]]))
        for x in r["lines"]:
            self.assertIn("voice", x)
            self.assertIn("start", x)

    @unittest.skipUnless(shutil.which("node"), "node needed")
    def test_the_sidecar_refuses_a_voice_not_in_the_cast_file(self):
        job = radio.voice_job(self.ep, self.cast)
        job["lines"][0]["voice"] = "af_sky"
        jp = Path(self.tmp.name) / "job.json"
        jp.write_text(json.dumps(job))
        p = subprocess.run(["node", str(REPO / "radio-voice" / "render.mjs"), "--fake-tts",
                            "--cast", str(CAST), str(jp), str(Path(self.tmp.name) / "x.mp3")],
                           capture_output=True, text=True)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("af_sky", p.stderr + p.stdout)


class CliCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fx = Fixture(Path(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def cli(self, *args):
        env = dict(os.environ)
        env.update(self.fx.env())
        env["QUORUM_RADIO_NOW"] = NOW.isoformat()
        return subprocess.run([sys.executable, str(HERE / "quorum_cli.py"), "radio"] + list(args),
                              env=env, capture_output=True, text=True)

    def test_radio_inputs_prints_json_over_the_last_24h_by_default(self):
        p = self.cli("inputs")
        self.assertEqual(p.returncode, 0, p.stderr)
        o = json.loads(p.stdout)
        self.assertEqual(o["since"][:16], "2026-10-16T02:00")
        on = [d["id"] for d in o["desks"] if d["on_air"]]
        self.assertEqual(on, ["host", "meridian", "bastion", "skeptic", "accountant"])

    def test_radio_inputs_since(self):
        p = self.cli("inputs", "--since", "2026-10-09T00:00Z")
        o = json.loads(p.stdout)
        self.assertIn("sidebar", [d["id"] for d in o["desks"] if d["on_air"]])

    def test_radio_tonight_fake_no_voice_writes_the_note(self):
        p = self.cli("tonight", "--fake", "--no-voice")
        self.assertEqual(p.returncode, 0, p.stderr)
        notes = list((self.fx.vault / "01_Projects" / "Quorum" / "Radio").glob("*.md"))
        self.assertEqual(len(notes), 1)
        self.assertIn("Night Recap (2026-10-17)", notes[0].name)
        self.assertIn(str(notes[0]), p.stdout)

    def test_unknown_radio_subcommand(self):
        p = self.cli("dance")
        self.assertEqual(p.returncode, 2)


class WallCase(unittest.TestCase):
    def test_the_stdlib_core_never_imports_radio(self):
        for f in ("quorum_cli.py", "vault_index.py", "repo_index.py", "spend_readout.py", "lint.py"):
            src = (HERE / f).read_text()
            self.assertNotRegex(src, r"(?m)^\s*(import radio|from radio)", f)
            self.assertNotIn("kokoro", src.lower(), f)

    def test_the_voice_sidecar_pins_kokoro_exactly(self):
        pkg = json.loads((REPO / "radio-voice" / "package.json").read_text())
        self.assertEqual(pkg["dependencies"]["kokoro-js"], "1.2.1")
        gi = (REPO / ".gitignore").read_text()
        self.assertIn("node_modules/", gi)


if __name__ == "__main__":
    unittest.main()
