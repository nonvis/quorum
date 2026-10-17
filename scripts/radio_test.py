#!/usr/bin/env python3
"""Tests for scripts/radio.py — `quorum radio <project>`: the cast file and its
voice lock, the projects file, one project's $0 inputs for the day, the
perspective round (on a fake model) with the Verifier's bounded evidence reads,
the episode note and the voice job handed to the sidecar.
Run: python3 scripts/radio_test.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))

import radio  # noqa: E402

CAST = REPO / "radio" / "cast.json"
LOCK = REPO / "radio" / "voices.lock"
import radio_store  # noqa: E402

# The projects file lives in the vault (QR-24); the fixture vault gets this copy.
PROJECTS_JSON = {'v': 1,
                 'note': "Radio's projects (QR-8, QR-24): briefing notes, vault folders, repos, concept note; `active` defaults to "
                         'true.',
                 'projects': {'meridian': {'name': 'Meridian',
                                           'notes': ['01_Projects/Meridian/08 - *.md'],
                                           'folders': ['01_Projects/Meridian'],
                                           'repos': ['meridian'],
                                           'concept': ['01_Projects/Meridian/00 - *.md']},
                              'permafrost': {'name': 'Permafrost',
                                             'notes': ['01_Projects/Permafrost/28 - *.md', '01_Projects/Loqua/MVP/00 - *.md'],
                                             'folders': ['01_Projects/Permafrost', '01_Projects/Loqua'],
                                             'repos': ['permafrost', 'loqua-passkey'],
                                             'concept': ['01_Projects/Permafrost/00 - *.md']},
                              'sidebar': {'name': 'Sidebar',
                                          'notes': ['01_Projects/Sidebar/13 - *.md'],
                                          'folders': ['01_Projects/Sidebar'],
                                          'repos': ['sidebar', 'sidebar-screen'],
                                          'concept': ['01_Projects/Sidebar/00 - *.md']},
                              'bastion': {'name': 'Bastion',
                                          'notes': ['01_Projects/Partners/Samsung/Bastion/21 - *.md',
                                                    '01_Projects/Partners/Samsung/Linear/00 - *.md'],
                                          'folders': ['01_Projects/Partners/Samsung'],
                                          'repos': [],
                                          'concept': ['01_Projects/Partners/Samsung/Bastion/00 - *.md']},
                              'quorum': {'name': 'Quorum',
                                         'notes': ['01_Projects/Quorum/15 - *.md'],
                                         'folders': ['01_Projects/Quorum'],
                                         'repos': ['quorum'],
                                         'concept': ['01_Projects/Quorum/00 - *.md']}}}


def git(cwd, *args, env=None):
    e = dict(os.environ)
    e.update({"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t",
              "GIT_COMMITTER_EMAIL": "t@t"})
    e.update(env or {})
    return subprocess.run(["git", "-C", str(cwd)] + list(args), env=e, check=True,
                          capture_output=True, text=True).stdout


def J(o):
    return json.dumps(o)


# ── the cast ────────────────────────────────────────────────────────────────

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

    def test_the_tracked_cast_is_the_perspective_table(self):
        """QR-9/QR-12: the perspective specialties, re-bound once."""
        cast = radio.load_cast(CAST, LOCK)
        got = {d["id"]: (d["name"], d["voice"], d["pan"]) for d in cast if d["recap"]}
        # Specialties added from the web later are brainstorm-only (QR-27): they never join this table.
        self.assertEqual(got, {
            "host": ("Host", "af_heart", "centre"),
            "builder": ("Builder", "am_michael", "left"),
            "architect": ("Architect", "bf_emma", "right"),
            "verifier": ("Verifier", "am_fenrir", "centre-left"),
            "operator": ("Operator", "am_puck", "centre-right"),
            "product": ("Product", "af_bella", "far right"),
        })
        for d in cast:
            if not d["recap"]:
                continue
            self.assertEqual(set(d) - {"pan_value", "recap"},
                             {"id", "name", "voice", "pan", "reads"} | ({"brainstorm"} if d["id"] != "host" else set()))
            if d["id"] != "host":
                self.assertTrue(d["reads"]["question"].endswith("?"))
                self.assertTrue(d["reads"]["never"])

    def test_the_lock_holds_only_the_new_ids_and_records_the_rebinding(self):
        lock = json.loads(LOCK.read_text())
        self.assertEqual(radio.effective_voices(lock), {d["id"]: d["voice"] for d in radio.load_cast(CAST, LOCK)})
        self.assertIn("QR-12", lock["note"])
        for gone in ("meridian", "permafrost", "sidebar", "bastion", "skeptic", "accountant"):
            self.assertNotIn(gone, lock["voices"])

    def test_the_tracked_lock_records_the_qr16_owner_override(self):
        lock = json.loads(LOCK.read_text())
        self.assertEqual(lock["voices"]["product"], "af_nicole")    # the original binding stays recorded
        ov = [o for o in lock["overrides"] if o["id"] == "product"]
        self.assertEqual(ov, [dict(ov[0], id="product", **{"from": "af_nicole", "to": "af_bella", "ruling": "QR-16"})])
        self.assertIn("af_nicole", radio.reserved_voices(lock))

    def write_lock(self, cast, lock):
        c = self.dir / "cast.json"
        c.write_text(json.dumps({"v": 1, "cast": cast}))
        lk = self.dir / "voices.lock"
        lk.write_text(json.dumps(lock))
        return c, lk

    def test_an_owner_override_needs_a_ruling_id(self):
        cast = [{"id": "p", "name": "P", "voice": "af_bella", "pan": "left", "reads": {}}]
        c, lk = self.write_lock(cast, {"voices": {"p": "af_nicole"}})              # a silent edit
        with self.assertRaisesRegex(radio.CastError, "never reassigned"):
            radio.load_cast(c, lk)
        c, lk = self.write_lock(cast, {"voices": {"p": "af_nicole"},
                                       "overrides": [{"id": "p", "from": "af_nicole", "to": "af_bella"}]})
        with self.assertRaisesRegex(radio.CastError, "ruling"):
            radio.load_cast(c, lk)
        c, lk = self.write_lock(cast, {"voices": {"p": "af_nicole"},
                                       "overrides": [{"id": "p", "from": "af_sky", "to": "af_bella", "ruling": "QR-16"}]})
        with self.assertRaisesRegex(radio.CastError, "from"):
            radio.load_cast(c, lk)
        c, lk = self.write_lock(cast, {"voices": {"p": "af_nicole"},
                                       "overrides": [{"id": "p", "from": "af_nicole", "to": "af_bella", "ruling": "QR-16"}]})
        self.assertEqual(radio.load_cast(c, lk)[0]["voice"], "af_bella")

    def test_an_overridden_voice_stays_reserved(self):
        cast = [{"id": "p", "name": "P", "voice": "af_bella", "pan": "left", "reads": {}},
                {"id": "q", "name": "Q", "voice": "af_nicole", "pan": "right", "reads": {}}]
        c, lk = self.write_lock(cast, {"voices": {"p": "af_nicole"},
                                       "overrides": [{"id": "p", "from": "af_nicole", "to": "af_bella", "ruling": "QR-16"}]})
        with self.assertRaisesRegex(radio.CastError, "reserved"):
            radio.load_cast(c, lk)

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
        p = {d["id"]: d["pan_value"] for d in radio.load_cast(CAST, LOCK)}
        self.assertEqual(p["host"], 0.0)
        self.assertLess(p["builder"], p["verifier"])
        self.assertLess(p["verifier"], 0)
        self.assertGreater(p["operator"], 0)
        self.assertLess(p["operator"], p["architect"])
        self.assertLess(p["architect"], p["product"])


class ProjectsCase(unittest.TestCase):
    def test_the_projects_file_is_read_from_the_vault(self):
        """QR-8, QR-24: the file is in the vault's 06_Metadata, each project active unless it says not."""
        with tempfile.TemporaryDirectory() as td:
            v = Path(td)
            (v / "06_Metadata").mkdir()
            d = json.loads(json.dumps(PROJECTS_JSON))
            d["projects"]["bastion"]["active"] = False
            (v / "06_Metadata" / "quorum-projects.json").write_text(json.dumps(d))
            pr = radio.load_projects(v)
            self.assertEqual(sorted(pr), ["bastion", "meridian", "permafrost", "quorum", "sidebar"])
            self.assertEqual(pr["sidebar"]["repos"], ["sidebar", "sidebar-screen"])
            self.assertTrue(pr["sidebar"]["active"])
            self.assertFalse(pr["bastion"]["active"])

    def test_the_repo_no_longer_tracks_a_projects_file(self):
        """QR-24: vault paths stay out of the public repo."""
        self.assertFalse((REPO / "radio" / "projects.json").exists())


# ── a fixture: a git vault + repos, "now" = 14:00 KST ──────────────────────

NOW = datetime(2026, 10, 17, 5, 0, tzinfo=timezone.utc)       # 14:00 KST
TODAY = datetime(2026, 10, 16, 15, 0, tzinfo=timezone.utc)     # 00:00 KST


class Fixture:
    def __init__(self, root):
        self.root = root
        self.home = root / "home"
        self.vault = self.home / "vault"
        self.memory = root / "memory"
        self.store_root = root / "store"
        self.store = radio_store.Store(self.store_root)
        for p in (self.vault / "06_Metadata", self.memory, self.home / "nonvis"):
            p.mkdir(parents=True)
        (self.vault / "06_Metadata" / "quorum-repos.txt").write_text("# allowlist\nsidebar\nmeridian\n")
        (self.vault / "06_Metadata" / "quorum-projects.json").write_text(json.dumps(PROJECTS_JSON, indent=2))
        # A daily Sidebar recap recorded earlier in the week, in the store (QR-19).
        e = self.store.new("recap", "sidebar", NOW - timedelta(days=2), {"title": "Sidebar (2026-10-15)",
                                                                         "label": "2026-10-15", "status": "ready"})
        self.store.write_script(e, {"lines": [{"i": 1, "speaker": "builder", "name": "Builder", "on_air": True,
                                               "text": "I know the moderator gate landed."}]}, "# x\n")
        self.store.keep(e, True)
        git(self.vault, "init", "-q", "-b", "main")
        self.commit("2026-09-01T00:00:00Z")
        old = NOW - timedelta(days=3)
        self.note("01_Projects/Sidebar/13 - Briefing — Where Sidebar Is (rolling).md",
                  "---\ntitle: B\nsummary: \"Sidebar's briefing\"\n---\n# S\n\n## §1 State\nThe violence gate is wired in; v0.5.0 stays default.\n\n"
                  "## §2 Rulings in force\n- SC-35 hold\n\n"
                  "## §3 Owed\n- listen to the radio\n- c3 operator wallet 7.10 SUI, floor 5: top up next week\n\n"
                  "## §4 Next moves\n- ship v0.6 as default\n\n"
                  "- spend: `quorum spend --session aaaa1111` = total $3.50 EST; window 2026-10-16T20:00Z → 23:00Z\n"
                  "- spend: `quorum spend --session bbbb2222` = total $9.00 EST; window 2026-10-10T01:00Z → 02:00Z\n",
                  NOW - timedelta(hours=2))
        self.note("01_Projects/Sidebar/57 - The Violence Gate Wired In (Build Log, 2026-10-17).md",
                  "---\ntitle: G\nsummary: \"The gate merged as 0c13802; 31 tests green\"\n---\n# G\n\nTests: 31 green.\n",
                  NOW - timedelta(hours=4))
        self.note("01_Projects/Sidebar/58 - Radio Moves to Quorum (Design, 2026-10-17).md",
                  "---\nsummary: \"Radio leaves Sidebar; three modes again\"\n---\n# D\n\nRuled: QR-5.\n",
                  NOW - timedelta(hours=3))
        self.note("01_Projects/Sidebar/00 - Concept.md", "---\nsummary: \"agents of different owners talk\"\n---\n# C\n", old)
        self.note("01_Projects/Quorum/18 - Radio — the Owner's Desks on Air (Design, 2026-10-17).md",
                  "---\nsummary: \"radio design\"\n---\n# R\n\nOld text about voices.\n", old)
        self.note("01_Projects/Quorum/18 - Radio — the Owner's Desks on Air (Design, 2026-10-17).md",
                  "---\nsummary: \"radio design\"\n---\n# R\n\nOld text about voices.\n"
                  "QR-5: Sidebar Radio is removed; Radio moves to Quorum.\n", NOW - timedelta(hours=2, minutes=30))
        self.note("01_Projects/Quorum/18 - Radio — the Owner's Desks on Air (Design, 2026-10-17).md",
                  "---\nsummary: \"Radio BUILT in Quorum; quorum radio <project> runs\"\nstatus: \"BUILT\"\n---\n# R\n\n"
                  "Old text about voices.\nQR-5: Sidebar Radio is removed; Radio moves to Quorum.\n", NOW - timedelta(hours=1, minutes=30))
        self.note("01_Projects/Sidebar/45 - Rooms (Design, 2026-10-13).md",
                  "---\nsummary: \"rooms\"\n---\n# Rooms\n\nRuled: rooms are salons.\n", datetime(2026, 10, 13, 2, 0, tzinfo=timezone.utc))
        self.note("01_Projects/Sidebar/45 - Rooms (Design, 2026-10-13).md",
                  "---\nsummary: \"rooms\"\n---\n# Rooms\n\nRuled: rooms are salons.\nRuled: old rooms do not carry over.\n",
                  datetime(2026, 10, 15, 2, 0, tzinfo=timezone.utc))
        self.note("01_Projects/Permafrost/77 - Desk Broker (Design, 2026-10-17).md",
                  "---\nsummary: \"broker\"\n---\n# B\n\nNothing about the other project.\n", NOW - timedelta(hours=2))
        self.note("01_Projects/Quorum/Radio/03 - Sidebar (2026-10-15).md",
                  "---\nsummary: \"episode\"\n---\n# E\n\n## Transcript\n\n| # | Speaker | Voice | Line | Source |\n|---|---|---|---|---|\n"
                  "| 1 | Builder | `am_michael` | I know the moderator gate landed. | c |\n\n## Program log\n", NOW - timedelta(days=2))
        self.note("Action Queue/90 - Sidebar — listen to Radio (2026-10-17).md", "x", NOW - timedelta(hours=1))
        self.note("Action Queue/91 - Meridian — something (2026-10-17).md", "x", NOW - timedelta(hours=1))
        self.note("01_Projects/Sidebar/40 - Old Note.md", "---\nsummary: old\n---\n# O\n", old)
        self.note("01_Projects/Meridian/08 - Briefing — Where Meridian Is (rolling).md",
                  "# M\n\n## 1 · State\nold\n", old)
        (self.memory / "MEMORY_VERIFICATION.md").write_text(
            "# V\n\n- [☠☠☠ TESTED ≠ CALLED](x.md) — 16 tests, 0 call sites\n- [☠ minor](y.md) — small\n")
        (self.memory / "MEMORY_VERIFICATION_2.md").write_text("")
        self.repo("sidebar", [("old sidebar work", "2026-10-10T00:00:00Z"),
                              ("Moderator: the same words are screened once", "2026-10-13T03:00:00Z"),
                              ("Moderator: a hold retry reuses the pass", "2026-10-13T04:00:00Z"),
                              ("Merge screen-v0.6-wire: the violence gate wired in", "2026-10-17T00:40:00Z"),
                              ("Shell: the Tune in card is gone", "2026-10-17T01:00:00Z", "ui/Shell.tsx")])
        self.repo("sidebar-screen", [("V1 violence gate candidate", "2026-10-17T00:39:00Z")])  # off the allowlist
        self.repo("meridian", [("old meridian work", "2026-10-10T00:00:00Z")])

    def note(self, rel, text, when):
        p = self.vault / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        self.commit(when.strftime("%Y-%m-%dT%H:%M:%SZ"))

    def commit(self, when):
        git(self.vault, "add", "-A")
        git(self.vault, "commit", "-q", "--allow-empty", "-m", "notes",
            env={"GIT_AUTHOR_DATE": when, "GIT_COMMITTER_DATE": when})

    def repo(self, name, commits):
        r = self.home / "nonvis" / name
        r.mkdir()
        git(r, "init", "-q", "-b", "main")
        for i, c in enumerate(commits):
            subject, when = c[0], c[1]
            f = r / (c[2] if len(c) > 2 else "src/f%d.txt" % i)
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text(subject + "\n")
            git(r, "add", ".")
            git(r, "commit", "-q", "-m", subject, env={"GIT_AUTHOR_DATE": when, "GIT_COMMITTER_DATE": when})

    def sha(self, repo, subject):
        for line in git(self.home / "nonvis" / repo, "log", "--format=%h %s").splitlines():
            if line.split(" ", 1)[1] == subject:
                return line.split(" ", 1)[0]

    def env(self):
        return {"HOME": str(self.home), "QUORUM_VAULT": str(self.vault), "QUORUM_MEMORY": str(self.memory),
                "QUORUM_RADIO_NOW": NOW.isoformat(), "QUORUM_RADIO_STORE": str(self.store_root)}

    def inputs(self, project="sidebar", since=TODAY, until=NOW, **kw):
        return radio.gather_project(project, radio.load_projects(self.vault), self.vault, self.memory,
                                    since, until, [self.home / "nonvis"], store=self.store, **kw)


class InputsCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fx = Fixture(Path(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def test_the_project_reads_todays_commits_in_every_repo_it_lists(self):
        i = self.fx.inputs()
        self.assertTrue(i["active"])
        subj = [(c["repo"], c["subject"]) for c in i["commits"]]
        self.assertIn(("sidebar", "Merge screen-v0.6-wire: the violence gate wired in"), subj)
        # sidebar-screen is off the index allowlist but on the project's list (QR-8).
        self.assertIn(("sidebar-screen", "V1 violence gate candidate"), subj)
        self.assertNotIn("old sidebar work", [s for _, s in subj])

    def test_the_vault_notes_changed_today_in_its_folder(self):
        i = self.fx.inputs()
        changed = [n["path"].rsplit("/", 1)[-1] for n in i["changed"]]
        self.assertIn("57 - The Violence Gate Wired In (Build Log, 2026-10-17).md", changed)
        self.assertNotIn("40 - Old Note.md", changed)
        self.assertNotIn("08 - Briefing — Where Meridian Is (rolling).md", changed)
        self.assertEqual([n["summary"] for n in i["changed"] if n["path"].endswith("(Build Log, 2026-10-17).md")],
                         ["The gate merged as 0c13802; 31 tests green"])

    def test_the_briefing_spend_and_owed_rows(self):
        i = self.fx.inputs()
        self.assertEqual([b["path"].rsplit("/", 1)[-1] for b in i["briefing"]],
                         ["13 - Briefing — Where Sidebar Is (rolling).md"])
        self.assertIn("SC-35", i["briefing"][0]["rulings"])   # rulings, for the Architect
        self.assertEqual([s["line"][:45] for s in i["spend"]], ["spend: `quorum spend --session aaaa1111` = to"])
        self.assertEqual(i["owed"][0], "- c3 operator wallet 7.10 SUI, floor 5: top up next week")  # money first
        self.assertIn("- ship v0.6 as default", i["owed"])  # Next moves are the Operator's too
        self.assertEqual([a["title"] for a in i["action_queue"]], ["90 - Sidebar — listen to Radio (2026-10-17)"])

    def test_a_project_with_nothing_today_is_inactive(self):
        i = self.fx.inputs("meridian")
        self.assertFalse(i["active"])
        self.assertEqual(i["commits"], [])
        self.assertEqual(i["changed"], [])

    def test_bastion_reads_no_repo(self):
        i = self.fx.inputs("bastion")
        self.assertEqual(i["repos"], [])

    def test_inputs_record_the_vault_rev(self):
        head = git(self.fx.vault, "rev-parse", "HEAD").strip()
        self.assertEqual(self.fx.inputs()["vault_rev"], head)

    def test_the_default_window_is_today_in_kst(self):
        self.assertEqual(radio.today_kst(NOW), TODAY)
        self.assertEqual(radio.today_kst(datetime(2026, 10, 16, 14, 59, tzinfo=timezone.utc)),
                         datetime(2026, 10, 15, 15, 0, tzinfo=timezone.utc))

    def test_qr17_ranges_in_kst(self):
        R = radio.parse_range
        kst = lambda y, m, d, h=0: datetime(y, m, d, h, tzinfo=radio.KST).astimezone(timezone.utc)
        self.assertEqual(R("today", NOW), (kst(2026, 10, 17), NOW, "2026-10-17"))
        self.assertEqual(R("yesterday", NOW), (kst(2026, 10, 16), kst(2026, 10, 17), "2026-10-16"))
        self.assertEqual(R("week", NOW), (kst(2026, 10, 11), NOW, "2026-10-11..2026-10-17"))
        self.assertEqual(R("2026-10-15", NOW), (kst(2026, 10, 15), kst(2026, 10, 16), "2026-10-15"))
        self.assertEqual(R("2026-10-12..2026-10-14", NOW), (kst(2026, 10, 12), kst(2026, 10, 15), "2026-10-12..2026-10-14"))
        self.assertEqual(R("2026-10-16..2026-10-17", NOW), (kst(2026, 10, 16), NOW, "2026-10-16..2026-10-17"))
        for bad in ("tomorrow", "2026-13-01", "2026-10-15..2026-10-12", "2026-10-20"):
            with self.assertRaises(ValueError):
                R(bad, NOW)

    def test_qr17_a_range_is_a_digest(self):
        week = self.fx.inputs(since=datetime(2026, 10, 10, 15, 0, tzinfo=timezone.utc), until=NOW, label="2026-10-11..2026-10-17")
        self.assertTrue(week["range"])
        self.assertEqual(week["label"], "2026-10-11..2026-10-17")
        self.assertEqual(week["per_day"], {"2026-10-13": 2, "2026-10-17": 3})
        self.assertIn("moderator", [t["theme"] for t in week["themes"]])
        self.assertEqual([e["title"] for e in week["episodes"]], ["Sidebar (2026-10-15)"])   # from the store (QR-19)
        self.assertIn("I know the moderator gate landed.", week["episodes"][0]["transcript"])
        cast = radio.load_cast(CAST, LOCK)
        b = radio.sources_for_specialty(cast, "builder", radio.project_sources(week))
        self.assertLessEqual(len([x for x in b if x["id"].startswith("T")]), 3)   # at most 3 themes
        self.assertIn("2026-10-13: 2", " ".join(x["text"] for x in b))
        self.assertFalse(self.fx.inputs()["range"])

    def test_spend_window_end_may_be_time_only_or_a_full_stamp(self):
        self.assertEqual(radio.spend_window_end("spend: x; window 2026-10-15T02:24Z → 09:07Z"),
                         datetime(2026, 10, 15, 9, 7, tzinfo=timezone.utc))
        self.assertEqual(radio.spend_window_end("spend: x; window 2026-10-15T09:32Z → 2026-10-16T10:25Z"),
                         datetime(2026, 10, 16, 10, 25, tzinfo=timezone.utc))
        self.assertEqual(radio.spend_window_end("spend: x; window 2026-10-15T23:30Z → 01:10Z"),
                         datetime(2026, 10, 16, 1, 10, tzinfo=timezone.utc))
        self.assertIsNone(radio.spend_window_end("spend: no window"))

    def test_each_specialty_reads_its_own_slice(self):
        """QR-13: commits → Builder; evidence notes + rules → Verifier; rulings and design
        notes → Architect; the user-visible surface + concept → Product; Owed/Next/AQ → Operator."""
        cast = radio.load_cast(CAST, LOCK)
        src = radio.project_sources(self.fx.inputs())
        sl = {sid: radio.sources_for_specialty(cast, sid, src) for sid in ("builder", "verifier", "architect", "product", "operator")}
        txt = lambda sid: " ".join(x["text"] for x in sl[sid])
        self.assertIn("Merge screen-v0.6-wire", txt("builder"))
        self.assertIn("31 tests green", txt("verifier"))
        self.assertIn("TESTED ≠ CALLED", txt("verifier"))
        self.assertIn("Radio leaves Sidebar", txt("architect"))
        self.assertIn("SC-35", txt("architect"))                      # the briefing's §Rulings (it changed today)
        self.assertIn("ui/Shell.tsx", txt("product"))
        self.assertIn("agents of different owners talk", txt("product"))
        self.assertIn("c3 operator wallet", txt("operator"))
        self.assertIn("listen to Radio", txt("operator"))
        self.assertNotIn("Merge screen-v0.6-wire", txt("product"))
        self.assertNotIn("31 tests green", txt("builder"))

    def test_r6_product_sees_the_merge_subjects_as_context_only(self):
        i = self.fx.inputs()
        self.assertEqual([m["subject"] for m in i["merges"]], ["Merge screen-v0.6-wire: the violence gate wired in"])
        cast = radio.load_cast(CAST, LOCK)
        prod = radio.sources_for_specialty(cast, "product", radio.project_sources(i))
        self.assertNotIn("Merge screen-v0.6-wire", " ".join(x["text"] for x in prod))   # not a citable source

    def test_the_slices_are_disjoint(self):
        """The one allowed overlap: Product's read-only merge context repeats merge subjects the
        Builder also sees (R6); it is context in Product's prompt, never a source it can cite."""
        cast = radio.load_cast(CAST, LOCK)
        src = radio.project_sources(self.fx.inputs())
        sids = ("builder", "verifier", "architect", "product", "operator")
        keys = {sid: {(x["kind"], x["ref"], x["text"]) for x in radio.sources_for_specialty(cast, sid, src)} for sid in sids}
        for i, a in enumerate(sids):
            self.assertTrue(keys[a] or a == "operator", a)
            for b in sids[i + 1:]:
                self.assertFalse(keys[a] & keys[b], (a, b))
                self.assertFalse({k[1] for k in keys[a]} & {k[1] for k in keys[b]}, (a, b))

    def test_breaks_are_detected_mechanically(self):
        """QR-14: a red gate/walk, a service down, a balance under its FAIL floor, a rollback."""
        self.assertEqual(self.fx.inputs()["breaks"], [])
        self.fx.note("01_Projects/Sidebar/13 - Briefing — Where Sidebar Is (rolling).md",
                     "# S\n\n## §3 Owed\n- c3 operator `0xca4e` **2.34 SUI**: health FAILs below 5\n- listen\n",
                     NOW - timedelta(minutes=30))
        br = self.fx.inputs()["breaks"]
        self.assertEqual([b["signal"] for b in br], ["balance below its FAIL floor"])
        self.assertIn("2.34 SUI", br[0]["text"])
        for text, signal in (("The live walk went RED at step 4.", "a red gate or walk"),
                             ("The screen sidecar is DOWN since 09:10; health checks fail.", "a service down"),
                             ("The c1 deploy was rolled back to r42.", "a deploy rolled back")):
            self.fx.note("01_Projects/Sidebar/59 - Walk (Build Log, 2026-10-17).md",
                         "---\nsummary: \"%s\"\n---\n# W\n" % text, NOW - timedelta(minutes=20))
            self.assertIn(signal, [b["signal"] for b in self.fx.inputs()["breaks"]], text)

    def test_daemon_code_is_not_the_user_visible_surface(self):
        for p, want in (("apps/daemon/src/index.ts", False), ("apps/web/src/Shell.tsx", True),
                        ("docs/runbook.md", True), ("README.md", True), ("src/cli/main.py", True),
                        ("packages/core/src/radio.ts", False)):
            self.assertEqual(bool(radio.SURFACE_PATH.search(p)), want, p)

    def test_a_fail_floor_row_already_there_before_the_window_is_no_break(self):
        """QR-14 'broke in the window': an Owed row carried over from before today is routine."""
        self.fx.note("01_Projects/Sidebar/13 - Briefing — Where Sidebar Is (rolling).md",
                     "# S\n\n## §3 Owed\n- c3 operator **2.34 SUI**: health FAILs below 5\n",
                     TODAY - timedelta(hours=5))
        self.fx.note("01_Projects/Sidebar/13 - Briefing — Where Sidebar Is (rolling).md",
                     "# S\n\n## §3 Owed\n- c3 operator **2.34 SUI**: health FAILs below 5\n- listen\n",
                     NOW - timedelta(minutes=30))
        self.assertEqual(self.fx.inputs()["breaks"], [])

    def test_a_run_prompt_is_no_evidence(self):
        self.fx.note("01_Projects/Sidebar/60 - Run Prompt — the Gate Walk (2026-10-17).md",
                     "---\nsummary: \"walk the gate\"\n---\n# R\n", NOW - timedelta(minutes=10))
        n = [x for x in self.fx.inputs()["changed"] if x["title"].startswith("60 - Run Prompt")][0]
        self.assertIsNone(n["class"])

    def test_r1_a_cross_project_decision_note_naming_the_project_counts(self):
        cast = radio.load_cast(CAST, LOCK)
        arch = " ".join(x["text"] for x in radio.sources_for_specialty(cast, "architect", radio.project_sources(self.fx.inputs())))
        self.assertIn("Radio moves to Quorum", arch)
        self.assertNotIn("Old text about voices", arch)            # today's diff, not the whole note
        self.assertNotIn("Nothing about the other project", arch)  # a cross note that does not name Sidebar

    def test_r3_a_cross_note_also_gives_its_current_state(self):
        cast = radio.load_cast(CAST, LOCK)
        x = [s for s in radio.sources_for_specialty(cast, "architect", radio.project_sources(self.fx.inputs()))
             if s["id"].startswith("X")][0]
        self.assertIn("current state (at the window's end): Radio BUILT in Quorum; quorum radio <project> runs; status: BUILT",
                      x["text"])

    def test_r4_a_range_ranks_dated_decisions_one_note_before_a_second(self):
        week = self.fx.inputs(since=datetime(2026, 10, 10, 15, 0, tzinfo=timezone.utc), until=NOW, label="2026-10-11..2026-10-17")
        cast = radio.load_cast(CAST, LOCK)
        arch = [s for s in radio.sources_for_specialty(cast, "architect", radio.project_sources(week))]
        for s in arch:
            self.assertRegex(s["text"], r"^\[2026-10-\d\d\] ", s["id"])
        refs = [s["ref"].split("#")[0] for s in arch]
        rooms = [i for i, r in enumerate(refs) if r.endswith("45 - Rooms (Design, 2026-10-13).md")]
        self.assertEqual(len(rooms), 2)                              # two days of the same note …
        others = {r for r in refs[:rooms[1]] if not r.endswith("45 - Rooms (Design, 2026-10-13).md")}
        self.assertGreaterEqual(len(others), 2)                       # … the second after every other note's first
        self.assertTrue(any(s["text"].startswith("[2026-10-15] ") and "old rooms do not carry over" in s["text"] for s in arch))

    def test_r4_product_reads_the_user_facing_diffs_and_the_builder_keeps_code(self):
        cast = radio.load_cast(CAST, LOCK)
        src = radio.project_sources(self.fx.inputs())
        prod = " ".join(x["text"] for x in radio.sources_for_specialty(cast, "product", src))
        build = " ".join(x["text"] for x in radio.sources_for_specialty(cast, "builder", src))
        self.assertIn("ui/Shell.tsx", prod)
        self.assertIn("+Shell: the Tune in card is gone", prod)   # the diff itself
        self.assertNotIn("ui/Shell.tsx", build)
        self.assertIn("src/f", build)                              # code files stay the Builder's

    def test_r5_breaks_are_case_insensitive_or_marked(self):
        for text, signal in (("the live walk went red at step 4.", "a red gate or walk"),
                             ("the screen sidecar is down since 09:10: health checks time out.", "a service down"),
                             ("BROKEN: the c1 box rejects every upload", "marked BROKEN"),
                             ("the screen daemon crashed twice overnight.", "a service down")):
            self.fx.note("01_Projects/Sidebar/59 - Walk (Build Log, 2026-10-17).md",
                         "---\nsummary: \"x\"\n---\n# W\n\n%s\n" % text, NOW - timedelta(minutes=20))
            self.assertIn(signal, [b["signal"] for b in self.fx.inputs()["breaks"]], text)

    def test_r3_the_day_has_themes(self):
        th = self.fx.inputs()["themes"]
        self.assertEqual(sum(t["count"] for t in th), 3)
        self.assertIn("merge", [t["theme"] for t in th] + ["merge"])
        self.assertTrue(all(t["theme"] == t["theme"].lower() for t in th))

    def test_a_state_name_in_code_is_no_break(self):
        self.fx.note("01_Projects/Sidebar/59 - Walk (Build Log, 2026-10-17).md",
                     "---\nsummary: \"x\"\n---\n# W\n\n- On restart, a `making` attempt becomes `crashed`; `status: down` is a state.\n"
                     "- A failed episode or a crashed one may be remade.\n",
                     NOW - timedelta(minutes=20))
        self.assertEqual(self.fx.inputs()["breaks"], [])

    def test_a_run_prompt_never_reports_a_break(self):
        self.fx.note("01_Projects/Sidebar/61 - Run Prompt — Overnight (2026-10-17).md",
                     "---\nsummary: \"x\"\n---\n# R\n\nNothing here needs a coin: c3 is down and the mac is held.\n",
                     NOW - timedelta(minutes=10))
        self.assertEqual(self.fx.inputs()["breaks"], [])

    def test_r5_a_stopped_or_down_box_without_a_failure_is_no_break(self):
        for text in ("c3 is down again; the marker was set and removed.", "The box is stopped at close (pf down).",
                     "c1 went down at close, box off for the weekend."):
            self.fx.note("01_Projects/Sidebar/59 - Walk (Build Log, 2026-10-17).md",
                         "---\nsummary: \"x\"\n---\n# W\n\n%s\n" % text, NOW - timedelta(minutes=20))
            self.assertEqual(self.fx.inputs()["breaks"], [], text)

    def test_r5_a_range_counts_only_markers_and_red_gates(self):
        wk = lambda: self.fx.inputs(since=datetime(2026, 10, 10, 15, 0, tzinfo=timezone.utc), until=NOW,
                                    label="2026-10-11..2026-10-17")["breaks"]
        for text, want in (("the screen daemon crashed twice overnight.", []),
                           ("c3 operator **2.34 SUI**: health FAILs below 5", []),
                           ("BROKEN: the c1 box rejects every upload", ["marked BROKEN"]),
                           ("the live walk went red at step 4.", ["a red gate or walk"])):
            # one note per case: a week window keeps every earlier case's added lines
            for old in (self.fx.vault / "01_Projects" / "Sidebar").glob("59 - *.md"):
                old.unlink()
            self.fx.note("01_Projects/Sidebar/59 - Walk %d (Build Log, 2026-10-17).md" % len(text),
                         "---\nsummary: \"x\"\n---\n# W\n\n%s\n" % text, NOW - timedelta(minutes=20))
            self.assertEqual([b["signal"] for b in wk()], want, text)

    def test_a_routine_red_word_is_no_break(self):
        self.fx.note("01_Projects/Sidebar/59 - Walk (Build Log, 2026-10-17).md",
                     "---\nsummary: \"the gate turns a reject into a hold; red-team notes; the store is down to one copy\"\n---\n# W\n",
                     NOW - timedelta(minutes=20))
        self.assertEqual(self.fx.inputs()["breaks"], [])


# ── the Verifier's evidence reads (QR-10) ──────────────────────────────────

class EvidenceCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fx = Fixture(Path(self.tmp.name))
        self.inp = self.fx.inputs()

    def tearDown(self):
        self.tmp.cleanup()

    def read(self, op):
        return radio.run_read(op, self.inp, self.fx.vault)

    def test_git_show_in_a_project_repo(self):
        sha = self.fx.sha("sidebar", "Merge screen-v0.6-wire: the violence gate wired in")
        r = self.read({"op": "show", "repo": "sidebar", "rev": sha})
        self.assertTrue(r["ok"], r)
        self.assertEqual(r["cmd"], "git -C ~/nonvis/sidebar show --stat %s" % sha)
        self.assertIn("violence gate", r["out"])

    def test_git_log_and_grep_and_a_vault_note(self):
        r = self.read({"op": "log", "repo": "sidebar-screen", "n": 5})
        self.assertTrue(r["ok"], r)
        self.assertIn("V1 violence gate candidate", r["out"])
        r = self.read({"op": "grep", "repo": "sidebar", "rev": "HEAD", "pattern": "violence"})
        self.assertTrue(r["ok"], r)
        self.assertIn("violence", r["out"])
        r = self.read({"op": "note", "path": "01_Projects/Sidebar/57 - The Violence Gate Wired In (Build Log, 2026-10-17).md",
                       "grep": "Tests"})
        self.assertTrue(r["ok"], r)
        self.assertIn("31 green", r["out"])

    def test_reads_outside_the_project_are_refused(self):
        for op in ({"op": "log", "repo": "meridian"},                     # another project's repo
                   {"op": "show", "repo": "sidebar", "rev": "--output=/tmp/x"},  # an option as a rev
                   {"op": "show", "repo": "sidebar", "rev": "HEAD", "path": "../../etc/passwd"},
                   {"op": "note", "path": "../outside.md"},
                   {"op": "note", "path": "01_Projects/Meridian/08 - Briefing — Where Meridian Is (rolling).md"},
                   {"op": "rm", "repo": "sidebar"}):
            r = self.read(op)
            self.assertFalse(r["ok"], op)
            self.assertIn("refused", r["out"])

    def test_a_grep_with_no_match_is_evidence_not_a_failure(self):
        r = self.read({"op": "grep", "repo": "sidebar", "rev": "HEAD", "pattern": "kokoro-worker"})
        self.assertTrue(r["ok"], r)
        self.assertEqual(r["out"], "(no matches)")

    def test_git_branch_contains(self):
        sha = self.fx.sha("sidebar", "Merge screen-v0.6-wire: the violence gate wired in")
        r = self.read({"op": "contains", "repo": "sidebar", "rev": sha})
        self.assertTrue(r["ok"], r)
        self.assertEqual(r["cmd"], "git -C ~/nonvis/sidebar branch --contains %s" % sha)
        self.assertIn("main", r["out"])

    def test_output_is_bounded(self):
        r = self.read({"op": "log", "repo": "sidebar", "n": 500})
        self.assertLessEqual(len(r["out"]), radio.READ_CHARS)


# ── the round ───────────────────────────────────────────────────────────────

class Recorder(radio.FakeModel):
    """The fake model with per-step overrides: {(role, step) | (role, specialty): reply |
    callable(view) | [one-shot replies…]}; records every call as ((role, key), view)."""

    def __init__(self, overrides=None):
        super().__init__()
        self.overrides = overrides or {}
        self.calls = []

    def __call__(self, role, system, user, view):
        key = (role, view.get("mode") if role in ("host", "bhost") else view.get("specialty"))
        self.calls.append((key, view))
        o = self.overrides.get(key)
        if isinstance(o, list):
            o = o.pop(0) if o else None
        if callable(o):
            o = o(view)
        if o is None:
            return super().__call__(role, system, user, view)
        return o


def on_air(ep):
    return [l for l in ep["lines"] if l["on_air"]]


def speakers(ep):
    return [l["speaker"] for l in on_air(ep) if l["speaker"] != "host"]


class RoundCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fx = Fixture(Path(self.tmp.name))
        self.cast = radio.load_cast(CAST, LOCK)
        self.inp = self.fx.inputs()

    def tearDown(self):
        self.tmp.cleanup()

    def run_ep(self, overrides=None, inp=None, **kw):
        m = Recorder(overrides)
        return radio.run_episode(inp or self.inp, self.cast, m, vault=self.fx.vault, memory=self.fx.memory, **kw), m

    def question(self, sid):
        return next(d for d in self.cast if d["id"] == sid)["reads"]["question"]

    def test_the_order_follows_the_questions_and_the_operator_is_off_by_default(self):
        ep, m = self.run_ep()
        self.assertEqual(speakers(ep), ["builder", "verifier", "architect", "product"])
        self.assertIn("operator: off", " ".join(ep["log"]))
        lines = on_air(ep)
        self.assertEqual(lines[0]["kind"], "open")
        self.assertEqual(lines[-1]["kind"], "signoff")
        self.assertLessEqual(ep["calls"]["total"], 14)

    def test_the_host_asks_the_question_itself_not_the_roles_name(self):
        ep, m = self.run_ep()
        lines = on_air(ep)
        for i, l in enumerate(lines):
            if l["speaker"] == "host" or i == 0:
                continue
            ask = lines[i - 1]
            self.assertEqual(ask["speaker"], "host")
            self.assertIn(self.question(l["speaker"]), ask["text"])
            self.assertNotIn(next(d for d in self.cast if d["id"] == l["speaker"])["name"], ask["text"])

    def test_the_host_opens_with_the_shape_of_the_day_never_the_news(self):
        """QR-15 R3: counts + themes; no commit subjects, no note titles."""
        ep, m = self.run_ep()
        k, view = m.calls[0]
        self.assertEqual(k, ("host", "open"))
        self.assertEqual(view["day"]["commits"], 3)
        self.assertEqual(view["day"]["notes"], len(self.inp["changed"]))
        self.assertTrue(view["day"]["themes"])
        self.assertNotIn("subjects", view["day"])
        self.assertTrue(on_air(ep)[0]["text"].endswith(self.question("builder")))

    def test_each_specialty_is_given_only_its_slice(self):
        ep, m = self.run_ep()
        for (role, sid), view in m.calls:
            if role != "desk":
                continue
            kinds = {x["kind"] for x in view["sources"]}
            allowed = set(next(d for d in self.cast if d["id"] == sid)["reads"]["sources"]) | {"line on air", "evidence"}
            self.assertLessEqual(kinds, allowed, sid)

    def test_operator_with_invites_it_last_before_the_sign_off(self):
        ep, m = self.run_ep(with_operator=True)
        self.assertEqual(speakers(ep)[-1], "operator")
        self.assertEqual(on_air(ep)[-1]["kind"], "signoff")
        _, view = [c for c in m.calls if c[0] == ("desk", "operator")][0]
        self.assertTrue({x["kind"] for x in view["sources"]} <= {"owed", "line on air"})
        self.assertFalse(view["on_break"])

    def test_operator_joins_uninvited_on_a_break_and_says_only_the_break(self):
        self.fx.note("01_Projects/Sidebar/13 - Briefing — Where Sidebar Is (rolling).md",
                     "# S\n\n## §3 Owed\n- c3 operator `0xca4e` **2.34 SUI**: health FAILs below 5\n- listen\n",
                     NOW - timedelta(minutes=30))
        ep, m = self.run_ep(inp=self.fx.inputs())
        self.assertEqual(speakers(ep)[-1], "operator")
        self.assertIn("operator joins: balance below its FAIL floor", " ".join(ep["log"]))
        system, view = [(c[1]) for c in m.calls if c[0] == ("desk", "operator")][0], None
        _, view = [c for c in m.calls if c[0] == ("desk", "operator")][0]
        self.assertTrue(view["on_break"])
        texts = [x["text"] for x in view["sources"] if x["kind"] != "line on air"]
        self.assertEqual(len(texts), 1)
        self.assertIn("2.34 SUI", texts[0])
        self.assertNotIn("listen", " ".join(texts))

    def test_a_specialty_with_an_empty_slice_is_not_asked(self):
        """Speaks only when: no design note changed and no ruling → no Architect."""
        inp = dict(self.inp, changed=[n for n in self.inp["changed"] if "Design" not in n["title"]],
                   briefing=[dict(b, in_window=False) for b in self.inp["briefing"]], cross=[])
        ep, m = self.run_ep(inp=inp)
        self.assertNotIn("architect", speakers(ep))
        self.assertNotIn(("desk", "architect"), [k for k, _ in m.calls])

    def test_no_opinion_here_is_free_and_normal(self):
        ep, m = self.run_ep({("desk", "architect"): J({"pass": True})})
        a = [l for l in on_air(ep) if l["speaker"] == "architect"]
        self.assertEqual([(l["kind"], l["text"]) for l in a], [("pass", "No opinion here.")])
        self.assertIn("product", speakers(ep))

    def test_the_cap_holds_and_the_last_call_is_the_sign_off(self):
        for cap in (14, 6, 4, 3):
            ep, m = self.run_ep(cap=cap, with_operator=True)
            self.assertLessEqual(ep["calls"]["total"], cap)
            self.assertEqual(m.calls[-1][0], ("host", "signoff"))

    def test_limits_answers_300_questions_200_signoff_told_its_limit(self):
        long = "The gate merged on main. " + "It also did many other things along the way. " * 12
        ep, m = self.run_ep({("desk", "builder"): J({"answer": long, "label": "grounded", "source": "C1"}),
                             ("host", "signoff"): J({"say": "Good night, Sang. " * 60})})
        b = [l for l in on_air(ep) if l["speaker"] == "builder"][0]
        self.assertLessEqual(len(b["text"]), 300)
        self.assertTrue(b["text"].endswith("."))
        for l in on_air(ep):
            if l["speaker"] == "host" and l["kind"] in ("ask", "repeat"):
                self.assertLessEqual(len(l["text"]), 200, l["text"])
        self.assertLessEqual(len(on_air(ep)[-1]["text"]), 480)
        sv = [v for k, v in m.calls if k == ("host", "signoff")][0]
        self.assertEqual(sv["limit"], radio.SIGNOFF_CHARS)

    def test_r2_no_prompt_echoes_on_air(self):
        ep, m = self.run_ep({("desk", "builder"): J({"answer": "I know the gate merged. That's not on air yet.",
                                                     "label": "grounded", "source": "C1"}),
                             ("desk", "verifier"): lambda v: J({"verdict": "proven", "answer": "Yes. The run is in the log.",
                                                                "source": "E1"})})
        b = [l for l in on_air(ep) if l["speaker"] == "builder"][0]
        self.assertEqual(b["text"], "I know the gate merged.")
        v = [l for l in on_air(ep) if l["speaker"] == "verifier"][0]
        self.assertEqual(v["text"], "Proven: the run is in the log.")
        for t in ("No opinion here, but", "Only what is not on air", "not on air yet"):
            self.assertNotIn(t.lower(), " ".join(l["text"].lower() for l in on_air(ep) if l["kind"] != "pass"))

    def test_r6_one_optional_follow_up_before_the_operator(self):
        fu = J({"action": "ask", "to": "builder", "question": "Which part of that was hardest?", "reason": "unsaid"})
        ep, m = self.run_ep({("host", "followup"): fu, ("desk", "builder"): [None, J({
            "answer": "My guess is the screen wiring.", "label": "inferred", "source": "C2"})]}, with_operator=True)
        sp = speakers(ep)
        self.assertEqual(sp[-2:], ["builder", "operator"])
        self.assertEqual(len([k for k, _ in m.calls if k == ("host", "followup")]), 1)
        ask = [l for l in on_air(ep) if l["kind"] == "followup"][0]
        self.assertEqual(ask["text"], "Which part of that was hardest?")

    def test_r6_no_follow_up_after_a_pass(self):
        ep, m = self.run_ep({("desk", "product"): J({"pass": True})})
        self.assertNotIn(("host", "followup"), [k for k, _ in m.calls])
        self.assertIn("GR-8", " ".join(ep["log"]))

    def test_r6_the_host_may_decline_the_follow_up(self):
        ep, m = self.run_ep()
        self.assertEqual(len([k for k, _ in m.calls if k == ("host", "followup")]), 1)
        self.assertFalse([l for l in ep["lines"] if l["kind"] == "followup"])

    def test_an_answer_without_a_resolvable_source_is_a_pass(self):
        ep, m = self.run_ep({("desk", "builder"): J({"answer": "Things.", "label": "grounded", "source": "Z9"})})
        self.assertEqual([l["kind"] for l in ep["lines"] if l["speaker"] == "builder"], ["pass"])

    def test_spoken_labels(self):
        self.assertTrue(radio.with_label("The gate merged.", "grounded").startswith("I know"))
        self.assertTrue(radio.with_label("It slips.", "inferred").lower().startswith("my guess"))
        self.assertTrue(radio.with_label("It merged.", "borrowed", "builder").startswith("The Builder says"))

    def test_the_first_failed_call_aborts_the_episode(self):
        def boom(role, system, user, view):
            raise radio.ModelError("claude: unknown option")
        with self.assertRaises(radio.ModelError):
            radio.run_episode(self.inp, self.cast, boom, vault=self.fx.vault, memory=self.fx.memory)

    # ── the Verifier (QR-10) ──

    def test_the_verifier_reads_before_it_speaks_and_cites_the_builders_line(self):
        ep, m = self.run_ep()
        keys = [k for k, _ in m.calls]
        i = keys.index(("verify", "verifier"))
        self.assertEqual(keys[i + 1], ("desk", "verifier"))
        v = [l for l in on_air(ep) if l["speaker"] == "verifier"][0]
        b = [l for l in on_air(ep) if l["speaker"] == "builder"][0]
        self.assertEqual(v["about"], b["i"])
        self.assertTrue(v["evidence"][0]["cmd"].startswith("git -C ~/nonvis/sidebar"))
        self.assertTrue(v["text"].startswith("Proven"), v["text"])
        self.assertEqual(v["verdict"], "proven")
        ask = on_air(ep)[on_air(ep).index(v) - 1]
        self.assertEqual(ask["text"], self.question("verifier"))   # spoken: the question only
        self.assertEqual(ask["about"], b["i"])                      # the line # is in the transcript

    def test_not_proven_names_the_one_check_or_is_not_aired(self):
        ep, m = self.run_ep({("desk", "verifier"): J({"verdict": "not_proven", "answer": "the merge shows no test run.",
                                                       "check": "run the screen e2e on the merge", "source": "E1"})})
        v = [l for l in on_air(ep) if l["speaker"] == "verifier"][0]
        self.assertTrue(v["text"].startswith("Not proven yet"), v["text"])
        self.assertIn("run the screen e2e on the merge", v["text"])
        self.assertIn(v["about"], ep["unverified"])
        ep, m = self.run_ep({("desk", "verifier"): J({"verdict": "not_proven", "answer": "I doubt it.", "source": "E1"})})
        self.assertEqual([l["kind"] for l in ep["lines"] if l["speaker"] == "verifier"], ["pass"])

    def test_a_verifier_that_read_nothing_does_not_speak(self):
        ep, m = self.run_ep({("verify", "verifier"): J({"reads": []})})
        self.assertEqual([l["kind"] for l in ep["lines"] if l["speaker"] == "verifier"], ["pass"])
        self.assertNotIn(("desk", "verifier"), [k for k, _ in m.calls])

    def test_verifier_reads_are_bounded(self):
        many = J({"reads": [{"op": "log", "repo": "sidebar", "n": 3}] * 9})
        ep, m = self.run_ep({("verify", "verifier"): many})
        v = [l for l in ep["lines"] if l["speaker"] == "verifier"][0]
        self.assertEqual(len(v["evidence"]), radio.MAX_READS)

    def test_r2_a_change_claim_is_proven_by_repository_evidence(self):
        ep, m = self.run_ep({("desk", "builder"): J({"answer": "I know the Tune in card was removed.", "label": "grounded",
                                                     "source": "C3"}),
                             ("verify", "verifier"): J({"reads": [{"op": "grep", "repo": "sidebar", "rev": "HEAD",
                                                                   "pattern": "kokoro-worker"}]}),
                             ("desk", "verifier"): J({"verdict": "proven", "answer": "no trace is left at HEAD.", "source": "E1"})})
        v = [l for l in on_air(ep) if l["speaker"] == "verifier"][0]
        self.assertEqual(v["claim_kind"], "change")
        self.assertTrue(v["text"].startswith("Proven"), v["text"])
        _, rv = [c for c in m.calls if c[0] == ("verify", "verifier")][0]
        self.assertEqual(rv["claim_kind"], "change")

    def test_r2_a_works_claim_needs_a_run_record_not_repository_evidence(self):
        works = J({"answer": "I know the gossip walk passes now.", "label": "grounded", "source": "C2"})
        ep, m = self.run_ep({("desk", "builder"): works,
                             ("desk", "verifier"): J({"verdict": "proven", "answer": "the commit is on main.", "source": "E1"})})
        v = [l for l in ep["lines"] if l["speaker"] == "verifier"][0]
        self.assertEqual(v["kind"], "pass")                        # a git show proves a change, not that it works
        self.assertIn("needs a run record", " ".join(ep["log"]))
        log = "01_Projects/Sidebar/57 - The Violence Gate Wired In (Build Log, 2026-10-17).md"
        ep, m = self.run_ep({("desk", "builder"): works,
                             ("verify", "verifier"): J({"reads": [{"op": "note", "path": log, "grep": "Tests"}]}),
                             ("desk", "verifier"): J({"verdict": "proven", "answer": "the build log shows 31 green.", "source": "E1"})})
        v = [l for l in on_air(ep) if l["speaker"] == "verifier"][0]
        self.assertEqual(v["claim_kind"], "works")
        self.assertTrue(v["text"].startswith("Proven"), v["text"])

    def test_r6_product_is_given_the_merges_as_context(self):
        ep, m = self.run_ep()
        _, pv = [c for c in m.calls if c[0] == ("desk", "product")][0]
        self.assertEqual(pv["merges"], ["sidebar: Merge screen-v0.6-wire: the violence gate wired in"])
        self.assertFalse([x for x in pv["sources"] if "Merge screen" in x["text"]])

    # ── QR7-1: the host asks; it never restates ──

    def test_a_host_question_is_a_question_only(self):
        ok = radio.host_question_ok
        for q in ("Is that actually proven?", "Product, does a user notice any of it?",
                  "And Product, which part was hardest?", "What did we decide today, and what did it cost?"):
            self.assertTrue(ok(q), q)
        for q in ("The README says Roundtable is built. Was that walk done today?",
                  "The README says Roundtable is built, was that walk done today?",
                  "So the gate merged?", "You said Radio moves to Quorum. Has that landed?",
                  "Product: the docs changed. Anything else?", "Good."):
            self.assertFalse(ok(q), q)
        for d in self.cast:
            if d["id"] != "host":
                self.assertTrue(ok(d["reads"]["question"]), d["id"])

    def test_a_restating_follow_up_is_re_asked_once(self):
        bad = J({"action": "ask", "to": "product", "question": "The README says it is walked. Was that today?"})
        good = J({"action": "ask", "to": "product", "question": "Product, when was that walk done?"})
        ep, m = self.run_ep({("host", "followup"): [bad, good]})
        fus = [v for k, v in m.calls if k == ("host", "followup")]
        self.assertEqual(len(fus), 2)
        self.assertIn("restate", fus[1]["retry"])
        self.assertEqual([l["text"] for l in on_air(ep) if l["kind"] == "followup"], ["Product, when was that walk done?"])

    def test_a_follow_up_that_still_restates_falls_back_to_the_template(self):
        bad = J({"action": "ask", "to": "product", "question": "The README says it is walked. Was that today?"})
        ep, m = self.run_ep({("host", "followup"): [bad, bad]})
        self.assertEqual([l["text"] for l in on_air(ep) if l["kind"] == "followup"], ["Product, can you say more about that?"])
        self.assertIn("template", " ".join(ep["log"]))

    # ── QR7-2: no merge state contradicting the window's merges ──

    def test_an_unmerged_claim_about_a_merged_thing_is_refused_and_re_asked_once(self):
        wrong = J({"answer": "I know the violence gate is only a candidate release, unmerged.", "label": "grounded", "source": "U1"})
        right = J({"answer": "I know the violence gate is now part of the screen.", "label": "grounded", "source": "U1"})
        ep, m = self.run_ep({("desk", "product"): [wrong, right]})
        calls = [v for k, v in m.calls if k == ("desk", "product")]
        self.assertEqual(len(calls), 2)
        self.assertIn("Merge screen-v0.6-wire", calls[1]["retry"])
        p = [l for l in on_air(ep) if l["speaker"] == "product"]
        self.assertEqual([l["text"] for l in p], ["I know the violence gate is now part of the screen."])
        ep, m = self.run_ep({("desk", "product"): [wrong, wrong]})
        self.assertEqual([l["kind"] for l in ep["lines"] if l["speaker"] == "product"], ["pass"])
        self.assertIn("contradicts a merge", " ".join(ep["log"]))

    def test_an_unmerged_claim_about_something_not_merged_stands(self):
        line = J({"answer": "I know the dark theme is still unmerged.", "label": "grounded", "source": "U1"})
        ep, m = self.run_ep({("desk", "product"): line})
        self.assertEqual(len([k for k, _ in m.calls if k == ("desk", "product")]), 1)
        self.assertIn("dark theme is still unmerged", " ".join(l["text"] for l in on_air(ep)))

    def test_a_not_proven_line_keeps_its_sentences_capitalised(self):
        ep, m = self.run_ep({("desk", "verifier"): J({"verdict": "not_proven", "answer": "The docs part is shown.",
                                                       "check": "grep for room", "source": "E1"})})
        v = [l for l in on_air(ep) if l["speaker"] == "verifier"][0]
        self.assertEqual(v["text"], "Not proven yet. The one check that settles it: grep for room. The docs part is shown.")

    def test_a_range_asks_its_questions_for_the_period(self):
        week = self.fx.inputs(since=datetime(2026, 10, 10, 15, 0, tzinfo=timezone.utc), until=NOW, label="2026-10-11..2026-10-17")
        ep, m = self.run_ep(inp=week)
        self.assertTrue(on_air(ep)[0]["text"].endswith("What actually changed this week?"), on_air(ep)[0]["text"])
        self.assertNotIn("today", " ".join(l["text"] for l in on_air(ep) if l["kind"] == "ask"))

    def test_a_not_proven_claim_is_put_back_and_its_withdrawal_stays_on_air(self):
        ep, m = self.run_ep({("desk", "verifier"): J({"verdict": "not_proven", "answer": "no run shown.",
                                                       "check": "run it", "source": "E1"}),
                             ("confirm", "builder"): J({"verdict": "withdraw", "say": ""})})
        b = [l for l in ep["lines"] if l["speaker"] == "builder"]
        self.assertTrue(b[0]["on_air"])
        self.assertTrue(b[0]["withdrawn"])
        kinds = [(l["speaker"], l["kind"]) for l in on_air(ep)]
        i = kinds.index(("host", "repeat"))
        self.assertEqual(kinds[i - 1], ("verifier", "answer"))
        self.assertEqual(kinds[i + 1], ("builder", "withdraw"))
        self.assertEqual(on_air(ep)[i + 1]["text"], "I can't source that, I withdraw it.")
        self.assertEqual(ep["withdrawn"], [b[0]["i"]])
        sv = [v for k, v in m.calls if k == ("host", "signoff")][0]
        self.assertEqual(sv["withdrawn"], [b[0]["i"]])

    def test_a_proven_claim_is_never_put_back(self):
        ep, m = self.run_ep()
        self.assertFalse([l for l in ep["lines"] if l["kind"] == "repeat"])


class NoteAndVoiceCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fx = Fixture(Path(self.tmp.name))
        self.cast = radio.load_cast(CAST, LOCK)
        self.inp = self.fx.inputs()
        self.ep = radio.run_episode(self.inp, self.cast, radio.FakeModel(), vault=self.fx.vault, memory=self.fx.memory)

    def tearDown(self):
        self.tmp.cleanup()

    def test_the_script_is_markdown_ready_to_paste(self):
        """QR-19: script.md is the export: frontmatter, the cast, one line per speaker turn with its source."""
        t = radio.script_md(self.ep, self.cast, {"kind": "recap", "title": "Sidebar (2026-10-17)",
                                                 "created": "2026-10-17T05:00:00Z"})
        fm = t.split("---")[1]
        for k in ('title: "Sidebar (2026-10-17)"', "date: 2026-10-17", "tags:", "summary:"):
            self.assertIn(k, fm)
        self.assertIn("calls: %d of 14" % self.ep["calls"]["total"], t)
        self.assertIn("vault: rev `%s`" % self.inp["vault_rev"], t)
        self.assertIn("## Script", t)
        self.assertIn("## Verifier reads", t)
        self.assertIn("git -C ~/nonvis/sidebar", t)
        for l in self.ep["lines"]:
            if l["on_air"]:
                self.assertIn(l["text"], t)
                self.assertIn(radio.voice_of(self.cast, l["speaker"]), t)

    def test_an_on_line_callback_sees_each_line_as_it_lands(self):
        seen = []
        ep = radio.run_episode(self.inp, self.cast, radio.FakeModel(), vault=self.fx.vault, memory=self.fx.memory,
                               on_line=seen.append)
        self.assertEqual([l["i"] for l in seen], [l["i"] for l in ep["lines"]])

    def test_the_voice_job_binds_each_line_to_its_speakers_voice_and_pan(self):
        job = radio.voice_job(self.ep, self.cast)
        on = [l for l in self.ep["lines"] if l["on_air"]]
        self.assertEqual(len(job["lines"]), len(on))
        by = {d["id"]: d for d in self.cast}
        for j, l in zip(job["lines"], on):
            self.assertEqual(j["voice"], by[l["speaker"]]["voice"])
            self.assertEqual(j["pan"], by[l["speaker"]]["pan_value"])
            self.assertGreater(j["pause"], 0)

    def test_the_voice_job_speaks_no_symbols(self):
        ep = {"lines": [{"i": 0, "speaker": "host", "kind": "open", "text": "Good ☠☠☠ evening ⭐ ✅ — all.",
                         "on_air": True}]}
        self.assertEqual(radio.voice_job(ep, self.cast)["lines"][0]["text"], "Good evening — all.")

    def test_pauses_come_from_structure(self):
        P = radio.pause_before
        q = {"speaker": "host", "kind": "ask", "text": "What moved?"}
        a = {"speaker": "builder", "kind": "answer", "text": "x"}
        s = {"speaker": "host", "kind": "signoff", "text": "Good night."}
        self.assertGreater(P(q, a), P(a, {"speaker": "architect", "kind": "answer", "text": "y"}))
        self.assertGreater(P(a, s), P(q, a))
        self.assertEqual(P(None, q), radio.PAUSES["lead"] * radio.NIGHT_STRETCH)

    @unittest.skipUnless(shutil.which("node") and shutil.which("ffmpeg") and shutil.which("ffprobe"),
                         "node + ffmpeg needed")
    def test_the_sidecar_renders_one_mp3_with_a_fake_tts(self):
        out = Path(self.tmp.name) / "ep.mp3"
        r = radio.render(self.ep, self.cast, out, fake_tts=True)
        self.assertTrue(out.exists())
        dur = float(subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration",
                                    "-of", "csv=p=0", str(out)], capture_output=True, text=True).stdout)
        self.assertAlmostEqual(dur, r["duration"], delta=0.5)
        self.assertEqual(len(r["lines"]), len([l for l in self.ep["lines"] if l["on_air"]]))

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
        return subprocess.run([sys.executable, str(HERE / "quorum_cli.py"), "radio"] + list(args),
                              env=env, capture_output=True, text=True)

    def episodes(self):
        return [m for m in self.fx.store.list(NOW) if m["title"] != "Sidebar (2026-10-15)"]

    def test_radio_project_fake_no_voice_writes_to_the_store_not_the_vault(self):
        """QR-19: the episode goes to the per-host store, unkept; nothing new in the vault."""
        p = self.cli("sidebar", "--fake", "--no-voice")
        self.assertEqual(p.returncode, 0, p.stderr)
        [m] = self.episodes()
        self.assertEqual((m["kind"], m["project"], m["title"], m["status"], m["kept"]),
                         ("recap", "sidebar", "Sidebar (2026-10-17)", "unvoiced", False))
        self.assertIn(m["id"], p.stdout)
        self.assertIn("Builder", self.fx.store.script_md(m["id"]))
        self.assertTrue(self.fx.store.script(m["id"])["lines"])
        self.assertEqual([x.name for x in (self.fx.vault / "01_Projects" / "Quorum" / "Radio").iterdir()],
                         ["03 - Sidebar (2026-10-15).md"])   # only the fixture's old episode note

    def test_events_stream_each_line_then_done(self):
        p = self.cli("sidebar", "--fake", "--no-voice", "--events")
        self.assertEqual(p.returncode, 0, p.stderr)
        ev = [json.loads(x) for x in p.stdout.splitlines()]
        self.assertEqual(ev[0]["event"], "started")
        self.assertEqual(ev[-1]["event"], "done")
        self.assertEqual(ev[-1]["id"], ev[0]["id"])
        lines = [e for e in ev if e["event"] == "line"]
        self.assertTrue(lines)
        self.assertEqual(lines[0]["line"]["speaker"], "host")
        self.assertIn("name", lines[0]["line"])

    def test_nothing_today_refuses_at_zero_cost(self):
        p = self.cli("meridian", "--no-voice")  # not --fake: a model call would fail the test env
        self.assertEqual(p.returncode, 1)
        self.assertIn("nothing on meridian today", p.stderr)
        self.assertEqual(self.episodes(), [])

    def test_an_inactive_project_is_refused(self):
        f = self.fx.vault / "06_Metadata" / "quorum-projects.json"
        d = json.loads(f.read_text())
        d["projects"]["sidebar"]["active"] = False
        f.write_text(json.dumps(d))
        p = self.cli("sidebar", "--fake", "--no-voice")
        self.assertEqual(p.returncode, 2)
        self.assertIn("inactive", p.stderr)

    def test_inputs_prints_the_project_json_for_today(self):
        p = self.cli("inputs", "sidebar")
        self.assertEqual(p.returncode, 0, p.stderr)
        o = json.loads(p.stdout)
        self.assertEqual(o["since"][:19], "2026-10-16T15:00:00")
        self.assertEqual(o["project"], "sidebar")

    def test_with_operator_and_an_unknown_with(self):
        p = self.cli("sidebar", "--fake", "--no-voice", "--with", "operator")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("| Operator | `am_puck` |", self.fx.store.script_md(self.episodes()[0]["id"]).split("## Script")[0])
        p = self.cli("sidebar", "--fake", "--no-voice", "--with", "builder")
        self.assertEqual(p.returncode, 2)

    def test_a_week_episode_is_titled_for_its_range(self):
        p = self.cli("sidebar", "week", "--fake", "--no-voice")
        self.assertEqual(p.returncode, 0, p.stderr)
        [m] = self.episodes()
        self.assertEqual((m["title"], m["label"]), ("Sidebar (2026-10-11..2026-10-17)", "2026-10-11..2026-10-17"))
        fm = self.fx.store.script_md(m["id"]).split("---")[1]
        self.assertIn("\ndate: 2026-10-17\n", fm)          # a range's script is dated by its last day

    def test_a_bad_range(self):
        p = self.cli("sidebar", "tomorrow", "--fake", "--no-voice")
        self.assertEqual(p.returncode, 2)
        self.assertIn("today | yesterday | week", p.stderr)

    def test_tonight_is_retired(self):
        p = self.cli("tonight")
        self.assertEqual(p.returncode, 2)
        self.assertIn("quorum radio <project>", p.stderr)

    def test_an_unknown_project(self):
        p = self.cli("dance")
        self.assertEqual(p.returncode, 2)
        self.assertIn("sidebar", p.stderr)


# ── brainstorm (QR-26, QR-27) ────────────────────────────────────────────────

QUESTION = "Should the moderator gate run before the screen or after it?"


def fake_search(q):
    return [{"path": "vault:01_Projects/Sidebar/58 - Radio Moves to Quorum (Design, 2026-10-17).md",
             "title": "Radio Moves to Quorum (Design, 2026-10-17)", "summary": "Radio leaves Sidebar", "snippet": "QR-5"},
            {"path": "vault:01_Projects/Sidebar/57 - The Violence Gate Wired In (Build Log, 2026-10-17).md",
             "title": "The Violence Gate Wired In (Build Log, 2026-10-17)", "summary": "31 tests green", "snippet": ""},
            {"path": "vault:01_Projects/Sidebar/00 - Concept.md", "title": "Concept", "summary": "agents talk", "snippet": ""},
            {"path": "vault:01_Projects/Sidebar/40 - Old Note.md", "title": "Old Note", "summary": "old", "snippet": ""},
            {"path": "memory:feedback_x.md", "title": "x", "summary": "a memory", "snippet": ""},
            {"path": "repo:sidebar:docs/moderator.md", "title": "moderator", "summary": "", "snippet": "the gate runs first"}]


class BrainstormCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.fx = Fixture(Path(self.tmp.name))
        self.cast = radio.load_cast(CAST, LOCK)
        self.inp = self.inputs()

    def tearDown(self):
        self.tmp.cleanup()

    def inputs(self, **kw):
        return radio.gather_brainstorm("sidebar", radio.load_projects(self.fx.vault), self.fx.vault, self.fx.memory,
                                       QUESTION, [self.fx.home / "nonvis"], search=fake_search, **kw)

    def run_bs(self, overrides=None, chosen=("builder", "verifier", "architect", "product"), **kw):
        m = Recorder(overrides)
        return radio.run_brainstorm(self.inp, self.cast, m, list(chosen), **kw), m

    def test_the_tracked_cast_gives_each_specialty_a_brainstorm_angle(self):
        for d in self.cast:
            if d["id"] != "host":
                self.assertTrue(d["brainstorm"]["angle"].endswith("?"), d["id"])
                self.assertTrue(d["brainstorm"]["sources"], d["id"])

    def test_the_sources_are_cut_into_disjoint_slices(self):
        src = radio.brainstorm_sources(self.inp)
        kinds = {s["kind"] for s in src}
        self.assertTrue({"code", "decision", "surface", "evidence", "hits"} <= kinds, kinds)
        by = lambda sid: {s["kind"] for s in radio.brainstorm_slice(self.cast, sid, src)}
        self.assertIn("code", by("builder"))
        self.assertIn("decision", by("architect"))
        self.assertIn("surface", by("product"))
        self.assertIn("evidence", by("verifier"))
        seen = {}
        for sid in ("builder", "verifier", "architect", "product", "operator"):
            for k in {d["id"]: d for d in self.cast}[sid]["brainstorm"]["sources"]:
                self.assertNotIn(k, seen, "%s and %s both read %s" % (seen.get(k), sid, k))
                seen[k] = sid

    def test_hits_are_classed_by_their_note(self):
        src = {s["ref"]: s["kind"] for s in radio.brainstorm_sources(self.inp)}
        self.assertEqual(src["01_Projects/Sidebar/58 - Radio Moves to Quorum (Design, 2026-10-17).md"], "decision")
        self.assertEqual(src["01_Projects/Sidebar/57 - The Violence Gate Wired In (Build Log, 2026-10-17).md"], "evidence")
        self.assertEqual(src["sidebar:docs/moderator.md"], "code")
        self.assertEqual(src["01_Projects/Sidebar/40 - Old Note.md"], "hits")
        self.assertEqual(src["memory:feedback_x.md"], "hits")

    def test_the_inputs_carry_the_question_the_repos_and_recent_commits(self):
        self.assertEqual(self.inp["question"], QUESTION)
        self.assertEqual([r["name"] for r in self.inp["repos"]], ["sidebar", "sidebar-screen"])
        self.assertTrue(any("violence gate" in c["subject"] for c in self.inp["commits"]))

    def test_a_session_opens_asks_everyone_chosen_and_signs_off_with_threads(self):
        ep, m = self.run_bs()
        on = on_air(ep)
        self.assertEqual((on[0]["speaker"], on[0]["kind"]), ("host", "open"))
        self.assertEqual((on[-1]["speaker"], on[-1]["kind"]), ("host", "signoff"))
        spoke = {l["speaker"] for l in on if l["speaker"] != "host"}
        self.assertEqual(spoke, {"builder", "verifier", "architect", "product"})
        self.assertTrue(ep["threads"])
        self.assertLessEqual(ep["calls"]["total"], 20)
        self.assertEqual(ep["kind"], "brainstorm")
        self.assertEqual(ep["question"], QUESTION)

    def test_only_the_chosen_specialties_sit(self):
        ep, m = self.run_bs(chosen=("builder", "product"))
        self.assertEqual({l["speaker"] for l in on_air(ep)} - {"host"}, {"builder", "product"})

    def test_a_host_pick_outside_the_room_goes_to_someone_unasked(self):
        ep, m = self.run_bs({("bhost", "next"): [J({"action": "ask", "to": "operator", "question": "What would it cost?"})]},
                            chosen=("builder", "product"))
        self.assertNotIn("operator", {l["speaker"] for l in on_air(ep)})

    def test_a_host_question_that_claims_something_is_replaced_by_the_angle(self):
        ep, m = self.run_bs({("bhost", "next"): [J({"action": "ask", "to": "product",
                                                     "question": "The gate is slow. Would users notice?"})]})
        asks = [l["text"] for l in on_air(ep) if l["speaker"] == "host" and l["kind"] == "ask"]
        self.assertNotIn("The gate is slow. Would users notice?", asks)
        self.assertTrue(any(a.startswith("Product, ") for a in asks), asks)

    def test_a_reply_names_the_line_it_answers(self):
        ep, m = self.run_bs({("bhost", "next"): [J({"action": "reply", "to": "architect", "about": 2,
                                                     "question": "Architect, does that fit what we ruled?"})]})
        a = next(l for l in on_air(ep) if l["speaker"] == "architect")
        self.assertEqual(a["about"], 2)

    def test_a_claim_without_a_source_is_said_as_a_guess(self):
        ep, m = self.run_bs({("bdesk", "product"): [J({"answer": "Users would feel the delay.", "label": "grounded",
                                                        "source": "none"})]})
        p = next(l for l in on_air(ep) if l["speaker"] == "product")
        self.assertTrue(p["text"].startswith("My guess is"), p["text"])
        self.assertEqual(p["label"], "inferred")

    def test_the_cap_holds_and_the_signoff_is_reserved(self):
        never = lambda v: J({"action": "ask", "to": "builder", "question": "What else would it take?"})
        ep, m = self.run_bs({("bhost", "next"): never}, cap=9)
        self.assertLessEqual(ep["calls"]["total"], 9)
        self.assertEqual(on_air(ep)[-1]["kind"], "signoff")

    def test_two_passes_in_a_row_wrap_the_session(self):
        pas = lambda v: J({"pass": True})
        ep, m = self.run_bs({("bdesk", s): pas for s in ("builder", "architect", "product", "verifier")}
                            | {("verify", "verifier"): J({"reads": []})})
        self.assertLess(ep["calls"]["total"], 12)
        self.assertIn("wrap", " ".join(ep["log"]))

    def test_earlier_scripts_are_context_for_everyone(self):
        ctx = [{"id": "x", "title": "Earlier", "question": "Before?", "transcript": "Builder: the gate exists."}]
        self.inp = self.inputs(context=ctx)
        ep, m = self.run_bs()
        for key, view in m.calls:
            if key[0] in ("bhost", "bdesk"):
                self.assertEqual(view.get("context"), ["x"], key)

    def test_the_verifier_reads_before_it_speaks(self):
        ep, m = self.run_bs()
        v = next(l for l in on_air(ep) if l["speaker"] == "verifier")
        self.assertTrue(v.get("evidence"))

    def test_the_script_lists_the_question_and_the_open_threads(self):
        ep, m = self.run_bs()
        t = radio.script_md(ep, self.cast, {"kind": "brainstorm", "title": QUESTION, "created": "2026-10-17T05:00:00Z"})
        self.assertIn("## Question", t)
        self.assertIn(QUESTION, t)
        self.assertIn("## Open threads", t)


class SpecialtyCase(unittest.TestCase):
    """QR-27: a specialty brought in from the web takes an unused voice for life."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        d = Path(self.tmp.name)
        self.cast_p, self.lock_p = d / "cast.json", d / "voices.lock"
        shutil.copy(CAST, self.cast_p)
        shutil.copy(LOCK, self.lock_p)

    def tearDown(self):
        self.tmp.cleanup()

    def add(self, **kw):
        s = {"id": "contrarian", "name": "Contrarian", "voice": "bm_george", "pan": "far left",
             "angle": "What is the strongest case against it?"}
        s.update(kw)
        return radio.add_specialty(s, self.cast_p, self.lock_p)

    def test_a_new_specialty_is_brainstorm_only_and_locked(self):
        self.add()
        cast = radio.load_cast(self.cast_p, self.lock_p)
        c = next(d for d in cast if d["id"] == "contrarian")
        self.assertFalse(c["recap"])
        self.assertEqual(c["brainstorm"], {"angle": "What is the strongest case against it?", "sources": ["hits"]})
        self.assertEqual(json.loads(self.lock_p.read_text())["voices"]["contrarian"], "bm_george")
        self.assertNotIn("contrarian", radio.ORDER)

    def test_a_held_or_retired_voice_is_refused(self):
        for v in ("af_heart", "af_nicole"):
            with self.assertRaises(radio.CastError):
                self.add(voice=v)

    def test_a_bad_id_an_existing_id_or_a_non_question_is_refused(self):
        for kw in ({"id": "Contra rian"}, {"id": "builder"}, {"angle": "be contrary"}, {"pan": "up"}):
            with self.assertRaises(radio.CastError):
                self.add(**kw)

    def test_free_voices_exclude_held_and_retired(self):
        free = radio.free_voices(self.cast_p, self.lock_p)
        self.assertNotIn("af_heart", free)
        self.assertNotIn("af_nicole", free)
        self.assertIn("bm_george", free)


class BrainstormCliCase(unittest.TestCase):
    setUp, tearDown, cli, episodes = CliCase.setUp, CliCase.tearDown, CliCase.cli, CliCase.episodes

    def test_brainstorm_writes_a_brainstorm_episode(self):
        p = self.cli("brainstorm", "sidebar", QUESTION, "--fake", "--no-voice", "--with", "builder,product")
        self.assertEqual(p.returncode, 0, p.stderr)
        [m] = self.episodes()
        self.assertEqual((m["kind"], m["question"], m["specialties"]), ("brainstorm", QUESTION, ["builder", "product"]))
        self.assertIn("## Open threads", self.fx.store.script_md(m["id"]))

    def test_brainstorm_refusals(self):
        self.assertEqual(self.cli("brainstorm", "sidebar", "", "--fake").returncode, 2)
        self.assertEqual(self.cli("brainstorm", "sidebar", QUESTION, "--fake", "--with", "dancer").returncode, 2)
        self.assertEqual(self.cli("brainstorm", "sidebar", QUESTION, "--fake", "--context", "nope").returncode, 2)

    def test_brainstorm_takes_an_earlier_script_as_context(self):
        first = self.fx.store.list(NOW)[0]["id"]
        p = self.cli("brainstorm", "sidebar", QUESTION, "--fake", "--no-voice", "--context", first)
        self.assertEqual(p.returncode, 0, p.stderr)
        m = next(x for x in self.episodes() if x["kind"] == "brainstorm")
        self.assertEqual(m["context"], [first])


class SpendCase(unittest.TestCase):
    def test_the_episode_spend_line_reads_the_session_dirs_window(self):
        with tempfile.TemporaryDirectory() as td:
            stub = Path(td) / "stub.py"
            stub.write_text("import json, sys\nprint(json.dumps({'status': 'ok', 'argv': sys.argv[1:], "
                            "'sessions_scanned': 26, 'records_counted': 13, 'total': {'est_usd': 0.1954}}))\n")
            line = radio.episode_spend(Path(td) / "session", "2026-10-17T02:20:00Z", script=stub)
        self.assertTrue(line.startswith("spend: `quorum spend --project "), line)
        self.assertIn("13 calls", line)
        self.assertIn("$0.20 EST", line)


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
        self.assertIn("node_modules/", (REPO / ".gitignore").read_text())


if __name__ == "__main__":
    unittest.main()
