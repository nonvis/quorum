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
        (self.vault / "Action Queue" / "15 - Meridian — parked rulings (2026-09-01).md").write_text("x")
        (self.vault / "Action Queue" / "20 - GCP migration (2026-09-04).md").write_text("x")
        git(self.vault, "init", "-q", "-b", "main")
        self.commit("2026-09-01T00:00:00Z")
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
                  "# B\n\n## 1. State\n#119 merged.\n\n## 3. Owed\n- BAS-242\n- top up the sweeper gas wallet: 2 SUI left, floor 5 SUI\n\n"
                  "- **10-16 session `cccc3333`:** spend: `quorum spend --session cccc3333` = total $2.00 EST; window 2026-10-16T21:00Z → 22:00Z\n", mtime=NOW - timedelta(hours=5))
        self.note("01_Projects/Partners/Samsung/Linear/00 - Ticket Ledger (rolling).md", "# T\n",
                  mtime=NOW - timedelta(days=3))
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
        """Writes a note and commits it at `mtime` (the git date is the evidence;
        the file's own mtime is left at now, so mtime can't be what counts)."""
        p = self.vault / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        self.commit(mtime.strftime("%Y-%m-%dT%H:%M:%SZ"))

    def commit(self, when):
        git(self.vault, "add", "-A")
        git(self.vault, "commit", "-q", "--allow-empty", "-m", "notes",
            env={"GIT_AUTHOR_DATE": when, "GIT_COMMITTER_DATE": when})

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
        self.assertIn("no git activity", self.desk(inp, "sidebar")["why"])

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

    def test_a_touched_file_is_not_activity(self):
        """QR2-2: file mtime no longer counts; only git evidence does."""
        p = self.fx.vault / "01_Projects/Sidebar/13 - Briefing — Where Sidebar Is (rolling).md"
        t = (NOW - timedelta(hours=1)).timestamp()
        os.utime(p, (t, t))
        self.assertFalse(self.desk(self.inputs(), "sidebar")["on_air"])

    def test_a_spend_line_alone_is_not_activity(self):
        self.fx.note("01_Projects/Sidebar/13 - Briefing — Where Sidebar Is (rolling).md",
                     "# S\n\n## §1 State\nx\n\n- spend: `quorum spend --session dddd4444` = total $1.00 EST; "
                     "window 2026-10-16T21:00Z → 22:00Z\n", mtime=NOW - timedelta(days=3))
        s = self.desk(self.inputs(), "sidebar")
        self.assertEqual(len(s["spend"]), 1)
        self.assertFalse(s["on_air"])

    def test_a_vault_commit_touching_the_desks_note_is_activity(self):
        b = self.desk(self.inputs(), "bastion")
        self.assertTrue(b["on_air"])
        self.assertIn("note commit", b["why"])
        self.assertTrue([n for n in b["notes"] if n["in_window"]])

    def test_as_of_reads_the_notes_at_the_window_end(self):
        """A replayed window reads each note as the vault held it at `until`."""
        rel = "01_Projects/Meridian/08 - Briefing — Where Meridian Is (rolling).md"
        self.fx.note(rel, "# M\n\n## 1 · State\nLATER TEXT\n", mtime=NOW + timedelta(hours=1))
        fx = self.fx
        then = radio.gather_inputs(self.cast, fx.vault, fx.memory, since=NOW - timedelta(hours=24),
                                   until=NOW, repo_roots=[fx.home / "nonvis"], as_of=True)
        live = self.inputs()
        m_then = self.desk(then, "meridian")["notes"][0]["excerpt"]
        self.assertIn("Ops/139", m_then)
        self.assertNotIn("LATER TEXT", m_then)
        self.assertIn("LATER TEXT", self.desk(live, "meridian")["notes"][0]["excerpt"])
        self.assertEqual(len(self.desk(then, "meridian")["spend"]), 1)

    def test_the_accountants_owed_rows_put_money_first(self):
        srcs = radio.desk_sources(self.desk(self.inputs(), "accountant"), self.fx.vault)
        owed = [x for x in srcs if x["kind"] == "owed"]
        self.assertIn("SUI", owed[0]["text"])
        self.assertLess([x["id"] for x in srcs].index(owed[0]["id"]),
                        [x["id"] for x in srcs].index([x for x in srcs if x["kind"] == "spend"][0]["id"]))

    def test_an_owed_row_carries_its_project(self):
        """QR3-4: the note's desk, unless the row names another desk."""
        self.fx.note("01_Projects/Sidebar/13 - Briefing — Where Sidebar Is (rolling).md",
                     "# S\n\n## §3 Owed\n- Permafrost c3 operator wallet 2.34 SUI, floor 5\n- listen\n",
                     mtime=NOW - timedelta(days=3))
        srcs = radio.desk_sources(self.desk(self.inputs(), "accountant"))
        owed = {x["text"].split(" (")[0]: x for x in srcs if x["kind"] == "owed"}
        self.assertEqual(owed["- Permafrost c3 operator wallet 2.34 SUI, floor 5"]["project"], "Permafrost")
        self.assertEqual(owed["- listen"]["project"], "Sidebar")
        self.assertEqual(owed["- BAS-242"]["project"], "Bastion")
        self.assertIn("project: Bastion", owed["- BAS-242"]["text"])

    def test_inputs_record_the_vault_rev(self):
        """QR3-3: the rev read, as of --until or HEAD for a live read."""
        head = git(self.fx.vault, "rev-parse", "HEAD").strip()
        self.assertEqual(self.inputs()["vault_rev"], head)
        self.assertFalse(self.inputs()["as_of"])

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


def J(o):
    return json.dumps(o)


class Recorder(radio.FakeModel):
    """The fake model with per-step overrides: {("host", mode) | ("desk", id) |
    ("confirm", id): reply | callable(view) | [replies…]}; records every call."""

    def __init__(self, overrides=None):
        super().__init__()
        self.overrides = overrides or {}
        self.calls = []

    def __call__(self, role, system, user, view):
        key = (role, view.get("mode") if role == "host" else view.get("desk"))
        self.calls.append((key, view))
        o = self.overrides.get(key)
        if isinstance(o, list):
            o = o.pop(0) if o else None
        if o is None:
            return super().__call__(role, system, user, view)
        return o(view) if callable(o) else o


def on_air(ep):
    return [l for l in ep["lines"] if l["on_air"]]


def speakers(ep):
    return [l["speaker"] for l in on_air(ep) if l["speaker"] != "host"]


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

    def run_ep(self, overrides=None, **kw):
        m = Recorder(overrides)
        return radio.run_episode(self.inp, self.cast, m, topic="daily night recap",
                                 vault=self.fx.vault, **kw), m

    # ── the shape (QR2-1) ──

    def test_the_fake_round_runs_projects_then_skeptic_then_cross_then_accountant(self):
        ep, m = self.run_ep()
        self.assertEqual(speakers(ep), ["meridian", "bastion", "skeptic", "bastion", "accountant"])
        self.assertLessEqual(ep["calls"]["total"], 14)
        self.assertEqual(on_air(ep)[0]["kind"], "open")
        self.assertEqual(on_air(ep)[-1]["kind"], "signoff")
        self.assertEqual(ep["cast"], ["host", "meridian", "bastion", "skeptic", "accountant"])

    def test_the_opening_question_goes_to_the_project_desks_only(self):
        ep, m = self.run_ep()
        keys = [k for k, _ in m.calls]
        first_order = keys.index(("host", "order"))
        self.assertEqual(keys[1:first_order], [("desk", "meridian"), ("desk", "bastion")])

    def test_one_host_call_orders_every_pick_with_a_bridge_and_a_reason(self):
        order = J({"order": [{"desk": "bastion", "bridge": "First, the Samsung side.", "reason": "money at a floor"},
                             {"desk": "meridian", "bridge": "Meridian, your night?", "reason": "the code"}],
                   "skeptic_question": "Skeptic, what do you doubt?"})
        ep, m = self.run_ep({("host", "order"): order})
        keys = [k for k, _ in m.calls]
        i = keys.index(("host", "order"))
        self.assertEqual(keys[i + 1], ("desk", "skeptic"))  # no host call between the picks
        lines = on_air(ep)
        self.assertEqual([(l["speaker"], l["kind"]) for l in lines[1:5]],
                         [("host", "pick"), ("bastion", "answer"), ("host", "pick"), ("meridian", "answer")])
        self.assertIn("Bastion", lines[1]["text"])  # the bridge names the desk even if the model did not
        self.assertEqual(lines[2]["reason"], "money at a floor")
        self.assertEqual(lines[4]["reason"], "the code")

    def test_an_answer_left_out_of_the_order_stays_off_air(self):
        order = J({"order": [{"desk": "meridian", "bridge": "Meridian?", "reason": "r"}]})
        ep, m = self.run_ep({("host", "order"): order})
        b = [l for l in ep["lines"] if l["speaker"] == "bastion" and l["kind"] == "answer"]
        self.assertEqual(speakers(ep)[0], "meridian")
        self.assertNotIn("bastion", speakers(ep)[:2])
        self.assertIn("drop: bastion", " ".join(ep["log"]))

    def test_the_skeptic_must_cite_a_line_on_air(self):
        ep, m = self.run_ep({("desk", "skeptic"): J({"answer": "Tested is not called.", "label": "grounded",
                                                     "source": "V1"})})
        sk = [l for l in on_air(ep) if l["speaker"] == "skeptic"]
        self.assertEqual([(l["kind"], l["text"]) for l in sk], [("pass", "No opinion here.")])

    def test_the_skeptic_citing_a_line_contests_it(self):
        ep, m = self.run_ep()
        sk = [l for l in on_air(ep) if l["speaker"] == "skeptic"][0]
        self.assertEqual(sk["source"]["kind"], "line on air")
        self.assertIn(int(sk["source"]["id"][1:]), ep["contested"])
        ask = [l for l in on_air(ep) if l["kind"] == "ask"][0]
        self.assertIn("Skeptic", ask["text"])

    def test_the_cross_desk_question_asks_one_desk_about_anothers_line(self):
        ep, m = self.run_ep()
        cross = [l for l in on_air(ep) if l["kind"] == "cross"]
        self.assertEqual(len(cross), 1)
        self.assertIn("Bastion", cross[0]["text"])
        ans = on_air(ep)[on_air(ep).index(cross[0]) + 1]
        self.assertEqual(ans["speaker"], "bastion")

    def test_a_cross_question_about_the_desks_own_line_is_refused(self):
        def own(view):
            line = [x for x in view["lines"] if x["desk"] == "meridian"][0]["line"]
            return J({"ask": {"to": "meridian", "about_line": line, "question": "Sure?"}, "say": "Meridian, sure?"})
        ep, m = self.run_ep({("host", "cross"): own})
        self.assertFalse([l for l in ep["lines"] if l["kind"] == "cross"])
        self.assertIn("cross: refused", " ".join(ep["log"]))
        self.assertEqual(speakers(ep)[-1], "accountant")

    def test_the_accountant_leads_with_the_owed_rows(self):
        ep, m = self.run_ep({("desk", "accountant"): lambda v: J({
            "answer": "Spend was three fifty.", "label": "grounded",
            "source": [s for s in v["sources"] if s["kind"] == "spend"][0]["id"]})})
        acc = [l for l in on_air(ep) if l["speaker"] == "accountant"]
        self.assertEqual([l["kind"] for l in acc], ["pass"])
        ep, m = self.run_ep()
        acc = [l for l in on_air(ep) if l["speaker"] == "accountant"][0]
        self.assertEqual(acc["source"]["kind"], "owed")
        self.assertIn("SUI", acc["source"]["text"])

    def test_a_host_turn_after_the_accountant_may_ask_one_desk_for_more_or_sign_off(self):
        ep, m = self.run_ep({("host", "turn"): [J({"action": "ask", "ask": {"to": "meridian", "question": "More on that?"},
                                                   "say": "Meridian, say more?"})]})
        lines = on_air(ep)
        self.assertEqual(lines[-3]["kind"], "ask")
        self.assertEqual(lines[-2]["speaker"], "meridian")
        self.assertEqual(lines[-1]["kind"], "signoff")

    # ── limits ──

    def test_gr8_no_further_question_when_the_opening_drew_fewer_than_two(self):
        ep, m = self.run_ep({("desk", "bastion"): J({"pass": True}),
                             ("host", "turn"): J({"action": "ask", "ask": {"to": "meridian", "question": "More?"}})})
        acc = [l for l in ep["lines"] if l["speaker"] == "accountant"][-1]
        self.assertFalse([l for l in ep["lines"][acc["i"]:] if l["kind"] == "ask" and l["on_air"]])
        self.assertIn("GR-8", " ".join(ep["log"]))
        self.assertEqual(on_air(ep)[-1]["kind"], "signoff")

    def test_the_cap_holds_and_the_last_call_is_the_sign_off(self):
        greedy = lambda v: J({"action": "ask", "ask": {"to": "meridian", "question": "More?"}, "say": "Meridian?"})
        ep, m = self.run_ep({("host", "turn"): greedy})
        self.assertLessEqual(ep["calls"]["total"], 14)
        self.assertEqual(m.calls[-1][0], ("host", "signoff"))
        ep, m = self.run_ep({("host", "turn"): greedy}, cap=9)
        self.assertLessEqual(ep["calls"]["total"], 9)
        self.assertEqual(m.calls[-1][0], ("host", "signoff"))

    def test_an_on_air_answer_is_at_most_300_characters(self):
        long = "The rewrite landed on main. " + "It also fixed many other things along the way. " * 12
        ep, m = self.run_ep({("desk", "meridian"): J({"answer": long, "label": "grounded", "source": "C1"})})
        mer = [l for l in on_air(ep) if l["speaker"] == "meridian"][0]
        self.assertLessEqual(len(mer["text"]), 300)
        self.assertTrue(mer["text"].endswith("."), mer["text"])
        self.assertTrue(mer["text"].startswith("I know"))
        self.assertTrue(radio.fit("x" * 400).endswith("."))
        self.assertLessEqual(len(radio.fit("word " * 100)), 300)

    def test_an_answer_without_a_resolvable_source_is_a_pass(self):
        bad = J({"answer": "Things happened.", "label": "grounded", "source": "N99"})
        nosrc = J({"answer": "Things happened.", "label": "grounded"})
        ep, m = self.run_ep({("desk", "meridian"): bad, ("desk", "bastion"): nosrc})
        self.assertFalse([l for l in ep["lines"] if l["kind"] == "answer" and l["speaker"] in ("meridian", "bastion")])
        self.assertFalse([k for k, _ in m.calls if k == ("host", "order")])
        self.assertEqual(on_air(ep)[-1]["kind"], "signoff")

    # ── repeat-back (QR2-1: only an unverified or contested claim) ──

    def test_a_repeat_back_of_an_uncontested_claim_is_ignored(self):
        def rb(view):
            line = [x for x in view["lines"] if x["desk"] == "bastion"][0]["line"]
            return J({"repeat_back": {"line": line, "claim": "PR 119 merged"},
                      "ask": {"to": "bastion", "about_line": [x for x in view["lines"] if x["desk"] == "meridian"][0]["line"],
                              "question": "Does that touch you?"}, "say": "Bastion, does that touch you?"})
        ep, m = self.run_ep({("host", "cross"): rb, ("desk", "skeptic"): J({"pass": True})})
        self.assertFalse([l for l in ep["lines"] if l["kind"] == "repeat"])
        self.assertIn("repeat-back refused", " ".join(ep["log"]))

    def test_a_repeat_back_of_a_contested_claim_is_put_to_its_desk(self):
        def rb(view):
            line = view["contested"][0]
            return J({"repeat_back": {"line": line, "claim": "the TFS rewrite landed"},
                      "ask": {"to": "bastion", "about_line": line, "question": "Does that touch you?"},
                      "say": "Bastion, does that touch you?"})
        ep, m = self.run_ep({("host", "cross"): rb})
        kinds = [(l["speaker"], l["kind"]) for l in on_air(ep)]
        i = kinds.index(("host", "repeat"))
        self.assertEqual(kinds[i + 1], ("meridian", "confirm"))
        self.assertTrue(on_air(ep)[i]["text"].startswith("So, Meridian, you're saying"))
        self.assertEqual(kinds[i + 2], ("host", "cross"))

    def test_a_withdrawal_stays_on_air_with_the_challenge(self):
        """QR3-1: claim, the Skeptic's doubt, the repeat-back and the withdrawal are all heard."""
        def rb(view):
            line = view["contested"][0]
            return J({"repeat_back": {"line": line, "claim": "the TFS rewrite landed"}, "say": "x"})
        ep, m = self.run_ep({("host", "cross"): [rb],
                             ("confirm", "meridian"): J({"verdict": "withdraw", "say": ""})})
        mer = [l for l in ep["lines"] if l["speaker"] == "meridian"]
        claim = mer[0]
        self.assertTrue(claim["on_air"])
        self.assertTrue(claim["withdrawn"])
        self.assertTrue(ep["lines"][claim["i"] - 1]["on_air"])  # its bridge too
        kinds = [(l["speaker"], l["kind"]) for l in on_air(ep)]
        self.assertIn(("skeptic", "answer"), kinds)
        i = kinds.index(("host", "repeat"))
        self.assertEqual(kinds[i + 1], ("meridian", "withdraw"))
        w = on_air(ep)[i + 1]
        self.assertEqual(w["text"], "I can't source that, I withdraw it.")
        self.assertEqual(ep["withdrawn"], [claim["i"]])

    def test_a_withdrawn_line_is_no_longer_a_target_or_a_source(self):
        seen = {}

        def rb(view):
            line = view["contested"][0]
            return J({"repeat_back": {"line": line, "claim": "the TFS rewrite landed"},
                      "ask": {"to": "bastion", "about_line": line, "question": "Does that touch you?"},
                      "say": "Bastion?"})

        def good(view):
            seen["standing"] = [x["line"] for x in view["lines"]]
            seen["withdrawn"] = view["withdrawn"]
            line = [x for x in view["lines"] if x["desk"] != "bastion"][0]["line"]
            return J({"ask": {"to": "bastion", "about_line": line, "question": "Does that touch you?"},
                      "say": "Bastion, does that touch you?"})

        def acc(view):
            seen["acc_sources"] = [x["id"] for x in view["sources"]]
            return None

        ep, m = self.run_ep({("host", "cross"): [rb, good],
                             ("confirm", "meridian"): J({"verdict": "withdraw", "say": "Not sure."}),
                             ("desk", "accountant"): acc})
        wd = ep["withdrawn"][0]
        self.assertNotIn(wd, seen["standing"])
        self.assertEqual(seen["withdrawn"], [wd])
        self.assertNotIn("L%d" % wd, seen["acc_sources"])

    def test_a_refused_cross_question_is_re_asked_once(self):
        """QR3-2: the program re-asks the host once within the budget."""
        def own(view):
            line = [x for x in view["lines"] if x["desk"] == "meridian"][0]["line"]
            return J({"ask": {"to": "meridian", "about_line": line, "question": "Sure?"}, "say": "Meridian?"})
        ep, m = self.run_ep({("host", "cross"): [own]})  # the second call falls to the fake: a valid pick
        crosses = [k for k, _ in m.calls if k == ("host", "cross")]
        self.assertEqual(len(crosses), 2)
        self.assertEqual(len([l for l in on_air(ep) if l["kind"] == "cross"]), 1)
        retry_view = [v for k, v in m.calls if k == ("host", "cross")][1]
        self.assertIn("refused", retry_view["retry"])
        ep, m = self.run_ep({("host", "cross"): own})  # refused twice: no third call
        self.assertEqual(len([k for k, _ in m.calls if k == ("host", "cross")]), 2)
        self.assertFalse([l for l in ep["lines"] if l["kind"] == "cross"])

    def test_the_sign_off_is_told_which_claims_were_withdrawn(self):
        def rb(view):
            return J({"repeat_back": {"line": view["contested"][0], "claim": "x"}, "say": "x"})
        ep, m = self.run_ep({("host", "cross"): [rb],
                             ("confirm", "meridian"): J({"verdict": "withdraw", "say": "Not sure."})})
        last_host = [v for k, v in m.calls if k[0] == "host"][-1]
        self.assertEqual(last_host["withdrawn"], ep["withdrawn"])

    # ── the rest ──

    def test_spoken_labels(self):
        self.assertTrue(radio.with_label("The rewrite landed.", "grounded").startswith("I know"))
        self.assertTrue(radio.with_label("It slips a week.", "inferred").lower().startswith("my guess"))
        self.assertTrue(radio.with_label("It was tested, not called.", "borrowed", "meridian")
                        .startswith("The Meridian desk says"))
        self.assertEqual(radio.with_label("I know the rewrite landed.", "grounded"),
                         "I know the rewrite landed.")

    def test_a_long_host_line_ends_at_a_sentence_not_mid_clause(self):
        long = "That is the night edition, Sang. " + "Meridian fixed a stale chain read on the box. " * 14
        ep, m = self.run_ep({("host", "signoff"): J({"say": long}),
                             ("host", "turn"): J({"action": "signoff", "say": long})})
        so = on_air(ep)[-1]
        self.assertEqual(so["kind"], "signoff")
        self.assertTrue(so["text"].endswith("."), so["text"][-40:])
        self.assertLessEqual(len(so["text"]), 480)

    def test_desks_no_longer_announce_themselves(self):
        """The host's bridge names the desk; the 300 characters are the claim's (QR2-7)."""
        ep, m = self.run_ep()
        for l in on_air(ep):
            if l["speaker"] != "host":
                self.assertNotRegex(l["text"], r"^(The )?\w+ here\.")

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

    def test_the_note_records_the_vault_rev_and_marks_withdrawn_claims(self):
        ep = dict(self.ep, vault_rev="abc1234def", as_of=True, withdrawn=[2])
        ep["lines"] = [dict(l) for l in self.ep["lines"]]
        ep["lines"][2]["withdrawn"] = True
        t = radio.write_note(ep, self.cast, self.fx.vault, day="2026-10-17").read_text()
        self.assertIn("vault: rev `abc1234def` (as of the window end)", t)
        row = [x for x in t.splitlines() if x.startswith("| 2 ")][0]
        self.assertIn("WITHDRAWN", row)

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

    def test_radio_inputs_until_replays_a_window(self):
        p = self.cli("inputs", "--since", "2026-10-09T00:00Z", "--until", "2026-10-15T00:00Z")
        self.assertEqual(p.returncode, 0, p.stderr)
        o = json.loads(p.stdout)
        self.assertEqual(o["until"][:16], "2026-10-15T00:00")
        self.assertTrue(o["as_of"])
        # 10-09..10-15 holds the old meridian/sidebar commits and the 10-14 note commits.
        self.assertEqual([d["id"] for d in o["desks"] if d["on_air"]],
                         ["host", "meridian", "permafrost", "sidebar", "bastion", "skeptic", "accountant"])
        m = [d for d in o["desks"] if d["id"] == "meridian"][0]
        self.assertEqual([c["subject"] for c in m["commits"]], ["old meridian work"])
        self.assertEqual(m["spend"], [])  # its spend line (10-16) is after the window

    def test_unknown_radio_subcommand(self):
        p = self.cli("dance")
        self.assertEqual(p.returncode, 2)


class SpendCase(unittest.TestCase):
    def test_the_episode_spend_line_reads_the_session_dirs_window(self):
        with tempfile.TemporaryDirectory() as td:
            stub = Path(td) / "stub.py"
            stub.write_text("import json, sys\nprint(json.dumps({'status': 'ok', 'argv': sys.argv[1:], "
                            "'sessions_scanned': 26, 'records_counted': 13, 'total': {'est_usd': 0.1954}}))\n")
            line = radio.episode_spend(Path(td) / "session", "2026-10-17T02:20:00Z", script=stub)
        self.assertTrue(line.startswith("spend: `quorum spend --project "), line)
        self.assertIn("--since 2026-10-17T02:20:00Z", line)
        self.assertIn("13 calls", line)
        self.assertIn("$0.20 EST", line)

    def test_a_failed_readout_says_so(self):
        with tempfile.TemporaryDirectory() as td:
            stub = Path(td) / "stub.py"
            stub.write_text("import sys\nsys.exit(2)\n")
            line = radio.episode_spend(Path(td), "2026-10-17T02:20:00Z", script=stub)
        self.assertIn("no readout", line)


class WallCase(unittest.TestCase):
    def test_the_stdlib_core_never_imports_radio(self):
        for f in ("quorum_cli.py", "vault_index.py", "repo_index.py", "spend_readout.py", "lint.py"):
            src = (HERE / f).read_text()
            self.assertNotRegex(src, r"(?m)^\s*(import radio|from radio)", f)
            self.assertNotIn("kokoro", src.lower(), f)

    def test_the_readme_has_no_vault_ref(self):
        import lint
        body = (REPO / "README.md").read_text()
        nofence = lint.FENCE.sub("", body)
        prose = lint.WIKI.sub(lambda m: m.group(0), lint.INLINE.sub("", nofence))
        self.assertIsNone(lint.VAULT_PROSE.search(prose))
        self.assertIsNone(lint.VAULT_CITE.search(nofence))

    def test_the_voice_sidecar_pins_kokoro_exactly(self):
        pkg = json.loads((REPO / "radio-voice" / "package.json").read_text())
        self.assertEqual(pkg["dependencies"]["kokoro-js"], "1.2.1")
        gi = (REPO / ".gitignore").read_text()
        self.assertIn("node_modules/", gi)


if __name__ == "__main__":
    unittest.main()
