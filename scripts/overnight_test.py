#!/usr/bin/env python3
"""Tests for overnight.py — `quorum overnight <project>` (vault Quorum Decision #128): one owner-started
headless session that runs the vault's overnight run prompt for one project, a per-host record of the
night, and a $0 score read from the vault's ledger note. Real temp dirs, the fake runner."""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import overnight  # noqa: E402

NOW = datetime(2026, 10, 29, 13, 0, tzinfo=timezone.utc)      # 22:00 KST, 10-29
RUN_PROMPT = "01_Projects/Quorum/30 - Run Prompt — The Overnight.md"
LEDGER = "01_Projects/Quorum/31 - Overnight Ledger.md"


def git(d, *args):
    subprocess.run(["git", "-C", str(d), "-c", "user.email=t@t", "-c", "user.name=t"] + list(args),
                   check=True, capture_output=True)


class Case(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.vault = root / "vault"
        (self.vault / "06_Metadata").mkdir(parents=True)
        (self.vault / "01_Projects" / "Quorum").mkdir(parents=True)
        (self.vault / "01_Projects" / "Permafrost").mkdir(parents=True)
        (self.vault / "01_Projects" / "Meridian").mkdir(parents=True)
        (self.vault / "00_Inbox").mkdir()
        (self.vault / "Action Queue").mkdir()
        projects = {
            "v": 1,
            "overnight": {"run_prompt": RUN_PROMPT, "ledger": LEDGER},
            "projects": {
                "permafrost": {"name": "Permafrost", "notes": ["01_Projects/Permafrost/28 - *.md"],
                               "folders": ["01_Projects/Permafrost"], "repos": ["permafrost"], "active": True},
                "meridian": {"name": "Meridian", "notes": ["01_Projects/Meridian/08 - *.md"],
                             "folders": ["01_Projects/Meridian"], "repos": ["meridian"], "active": True},
                "crucible": {"name": "Crucible", "notes": [], "folders": ["01_Projects/Crucible"],
                             "repos": [], "active": False},
            },
        }
        (self.vault / "06_Metadata" / "quorum-projects.json").write_text(json.dumps(projects))
        (self.vault / RUN_PROMPT).write_text(
            "---\ntitle: Run Prompt — The Overnight\n---\n# The Overnight\n\n## §0 Walls\n\n"
            "## Project: permafrost\n\nwalls and gap sources for Permafrost\n\n## §5 The morning\n")
        (self.vault / LEDGER).write_text(
            "---\ntitle: Overnight Ledger\n---\n# Overnight Ledger\n\n"
            "| Night | Project | Draft | Lane | Verdict | Landed |\n|---|---|---|---|---|---|\n"
            "| 2026-10-28 n1 | permafrost | Inbox/00 PII posture | research | Keep | Research/16 |\n"
            "| 2026-10-28 n1 | permafrost | Inbox/01 mainnet facts | research | Keep | Research/17 |\n"
            "| 2026-10-29 n3 | permafrost | Inbox/00 EDPB sources | depth | Discard | — |\n"
            "| 2026-10-29 n3 | permafrost | Inbox/03 buyer shape | research |  |  |\n"
            "| 2026-10-30 n4 | meridian | Inbox/00 ITCH attribution | research | Keep | Research/00 |\n")
        (self.vault / "01_Projects" / "Permafrost" / "28 - Briefing.md").write_text("# PF\n")
        git(self.vault, "init", "-q", "-b", "main")
        git(self.vault, "add", "-A")
        git(self.vault, "commit", "-qm", "init")
        self.store = overnight.Store(root / "store")
        self.runner = overnight.FakeRunner()

    def tearDown(self):
        self.tmp.cleanup()


class PlanTest(Case):
    def test_a_project_resolves_to_its_appendix(self):
        p = overnight.plan(self.vault, "permafrost")
        self.assertEqual(p["name"], "Permafrost")
        self.assertEqual(p["run_prompt"], RUN_PROMPT)
        self.assertEqual(p["ledger"], LEDGER)
        self.assertEqual(p["folders"], ["01_Projects/Permafrost"])
        self.assertTrue(p["run_prompt_path"].is_file())

    def test_no_appendix_no_night(self):
        with self.assertRaises(overnight.OvernightError) as cm:
            overnight.plan(self.vault, "meridian")
        self.assertIn("## Project: meridian", str(cm.exception))

    def test_unknown_and_inactive_projects_are_refused(self):
        with self.assertRaises(overnight.OvernightError):
            overnight.plan(self.vault, "nope")
        with self.assertRaises(overnight.OvernightError):
            overnight.plan(self.vault, "crucible")

    def test_the_projects_file_must_name_the_run_prompt(self):
        f = self.vault / "06_Metadata" / "quorum-projects.json"
        d = json.loads(f.read_text())
        del d["overnight"]
        f.write_text(json.dumps(d))
        with self.assertRaises(overnight.OvernightError) as cm:
            overnight.plan(self.vault, "permafrost")
        self.assertIn("overnight", str(cm.exception))


class CommandTest(Case):
    def test_the_command_runs_headless_from_the_vault_with_the_walls_denied(self):
        p = overnight.plan(self.vault, "permafrost")
        r = overnight.Runner()
        cmd = r.command(p, self.vault, "0123abcd-0000-4000-8000-000000000000")
        self.assertEqual(cmd[:2], [r.claude, "-p"])
        self.assertIn("--session-id", cmd)
        self.assertEqual(cmd[cmd.index("--session-id") + 1], "0123abcd-0000-4000-8000-000000000000")
        self.assertEqual(cmd[cmd.index("--model") + 1], overnight.MODEL)
        self.assertEqual(cmd[cmd.index("--permission-mode") + 1], "acceptEdits")
        self.assertEqual(cmd[cmd.index("--max-budget-usd") + 1], overnight.BUDGET_USD)
        self.assertEqual(cmd[cmd.index("--output-format") + 1], "stream-json")
        allowed = cmd[cmd.index("--allowedTools") + 1]
        for t in ("Agent", "WebFetch", "WebSearch", "Write", "Bash(git add:*)", "Bash(quorum:*)"):
            self.assertIn(t, allowed)
        denied = cmd[cmd.index("--disallowedTools") + 1]
        for t in ("Bash(gcloud:*)", "Bash(docker:*)", "Bash(git checkout:*)", "Bash(git stash:*)", "Bash(ssh:*)"):
            self.assertIn(t, denied)
        self.assertNotIn("--add-dir", cmd)                      # the vault is the cwd; repos stay read-only

    def test_only_the_vaults_project_settings_reach_the_night(self):
        # A host's `.claude/settings.local.json` (a bare "Bash" on the MSI, `scripts/pf up *`) and the user's
        # `~/.claude/settings.json` (a bare "Bash", "Write", "Edit" on the Mac) collect allows from hand sessions;
        # loaded into the night, they outrank the walls ALLOWED was written to be.
        cmd = overnight.Runner().command(overnight.plan(self.vault, "permafrost"), self.vault, "0123abcd-0000-4000-8000-000000000000")
        self.assertIn("--setting-sources", cmd)
        self.assertEqual(cmd[cmd.index("--setting-sources") + 1], "project")  # the vault's hooks + tracked allows

    def test_the_effort_is_set_by_the_verb_not_the_users_settings(self):
        cmd = overnight.Runner().command(overnight.plan(self.vault, "permafrost"), self.vault, "0123abcd-0000-4000-8000-000000000000")
        self.assertIn("--effort", cmd)
        self.assertEqual(cmd[cmd.index("--effort") + 1], overnight.EFFORT)

    def test_a_night_never_starts_a_night_or_a_model_verb(self):
        # Bash(quorum:*) is for lookups, lint and curate; the run prompt quotes `quorum overnight <project>` itself.
        cmd = overnight.Runner().command(overnight.plan(self.vault, "permafrost"), self.vault, "0123abcd-0000-4000-8000-000000000000")
        denied = cmd[cmd.index("--disallowedTools") + 1].split(",")
        for t in ("Bash(quorum overnight:*)", "Bash(quorum radio:*)", "Bash(quorum web:*)"):
            self.assertIn(t, denied)

    def test_the_prompt_names_the_note_the_appendix_and_the_session(self):
        p = overnight.plan(self.vault, "permafrost")
        text = overnight.user_prompt(p, "0123abcd-0000-4000-8000-000000000000", NOW)
        self.assertIn(RUN_PROMPT, text)
        self.assertIn("## Project: permafrost", text)
        self.assertIn("0123abcd", text)
        self.assertIn("2026-10-29 22:00 KST", text)
        self.assertNotIn("Sang is asleep", text)                # the note says what it is; the prompt only starts it


class NightTest(Case):
    def test_a_night_records_its_drafts_report_cost_and_session(self):
        events = []
        row = overnight.run(self.vault, "permafrost", self.runner, self.store, NOW, on_event=events.append)
        self.assertEqual(row["project"], "permafrost")
        self.assertEqual(row["date"], "2026-10-29")
        self.assertEqual(row["status"], "done")
        self.assertEqual(row["drafts"], ["00_Inbox/00 - Research draft — A (2026-10-29).md",
                                         "00_Inbox/01 - Depth draft — B (2026-10-29).md"])
        self.assertEqual(row["report"], "Action Queue/99 - Morning report — overnight, night 4: Permafrost (2026-10-29).md")
        self.assertAlmostEqual(row["cost"], 12.5)
        self.assertEqual(row["tools"], 4)
        self.assertEqual(len(row["session"]), 36)
        self.assertEqual(row["run_prompt"], RUN_PROMPT)
        self.assertEqual(self.runner.calls[0]["cwd"], str(self.vault))
        self.assertIn("## Project: permafrost", self.runner.calls[0]["prompt"])
        saved = self.store.night("permafrost", "2026-10-29")
        self.assertEqual(saved["session"], row["session"])
        self.assertEqual([e["event"] for e in events][:2], ["started", "tool"])
        self.assertEqual(events[-1]["event"], "done")

    def test_a_draft_copied_in_by_bash_counts_another_projects_does_not(self):
        def draft(name, project, date):
            (self.vault / "00_Inbox" / name).write_text(
                '---\ntitle: "x"\ntags: [%s, research, overnight-draft]\nstatus: "DRAFT — overnight %s (Quorum/30)"\n---\n# x\n'
                % (project, date), encoding="utf-8")
        draft("10 - Research draft — copied (2026-10-29).md", "permafrost", "2026-10-29")
        draft("11 - Research draft — other project (2026-10-29).md", "meridian", "2026-10-29")
        draft("12 - Research draft — last night (2026-10-28).md", "permafrost", "2026-10-28")
        row = overnight.run(self.vault, "permafrost", self.runner, self.store, NOW)
        self.assertEqual(row["drafts"], ["00_Inbox/00 - Research draft — A (2026-10-29).md",
                                         "00_Inbox/01 - Depth draft — B (2026-10-29).md",
                                         "00_Inbox/10 - Research draft — copied (2026-10-29).md"])

    def test_a_done_night_refuses_a_second_run_the_same_day(self):
        overnight.run(self.vault, "permafrost", self.runner, self.store, NOW)
        with self.assertRaises(overnight.OvernightError) as cm:
            overnight.run(self.vault, "permafrost", self.runner, self.store, NOW)
        self.assertIn("2026-10-29", str(cm.exception))
        self.assertEqual(len(self.runner.calls), 1)

    def test_a_failed_night_is_recorded_and_may_be_rerun(self):
        self.runner.fail = "claude -p exit 1: boom"
        with self.assertRaises(overnight.OvernightError):
            overnight.run(self.vault, "permafrost", self.runner, self.store, NOW)
        row = self.store.night("permafrost", "2026-10-29")
        self.assertEqual(row["status"], "error")
        self.assertIn("boom", row["error"])
        self.runner.fail = None
        row = overnight.run(self.vault, "permafrost", self.runner, self.store, NOW)
        self.assertEqual(row["status"], "done")

    def test_dry_run_prints_and_writes_nothing(self):
        row = overnight.run(self.vault, "permafrost", self.runner, self.store, NOW, dry_run=True)
        self.assertEqual(row["status"], "dry-run")
        self.assertEqual(self.runner.calls, [])
        self.assertEqual(self.store.nights(), [])

    def test_list_is_newest_first_across_projects(self):
        overnight.run(self.vault, "permafrost", self.runner, self.store, NOW)
        (self.vault / RUN_PROMPT).write_text((self.vault / RUN_PROMPT).read_text() + "\n## Project: meridian\n\nx\n")
        overnight.run(self.vault, "meridian", self.runner, self.store, NOW.replace(day=30))
        rows = self.store.nights()
        self.assertEqual([(r["project"], r["date"]) for r in rows], [("meridian", "2026-10-30"), ("permafrost", "2026-10-29")])


class ContextTest(Case):
    def test_todays_commits_under_the_project_folders(self):
        self.assertEqual(overnight.commits_since(self.vault, ["01_Projects/Permafrost"], "2000-01-01T00:00:00+09:00"), 1)
        # git's date parser treats a year >= 2100 as "now" and returns everything; 2099 is a real future
        self.assertEqual(overnight.commits_since(self.vault, ["01_Projects/Permafrost"], "2099-01-01T00:00:00+09:00"), 0)
        self.assertEqual(overnight.commits_since(self.vault, ["01_Projects/Meridian"], "2000-01-01T00:00:00+09:00"), 0)

    def test_kst_midnight(self):
        self.assertEqual(overnight.kst_midnight(NOW).isoformat(), "2026-10-29T00:00:00+09:00")
        self.assertEqual(overnight.kst_date(NOW), "2026-10-29")


class ScoreTest(Case):
    def test_score_reads_the_ledger_per_project(self):
        s = overnight.score(self.vault, LEDGER)
        self.assertEqual(s["permafrost"], {"drafted": 4, "kept": 2, "discarded": 1, "pending": 1,
                                           "by_lane": {"research": {"drafted": 3, "kept": 2, "discarded": 0, "pending": 1},
                                                       "depth": {"drafted": 1, "kept": 0, "discarded": 1, "pending": 0}}})
        self.assertEqual(s["meridian"]["kept"], 1)
        self.assertEqual(overnight.score(self.vault, LEDGER, "meridian").keys(), {"meridian"})

    def test_a_missing_ledger_scores_nothing(self):
        with self.assertRaises(overnight.OvernightError):
            overnight.score(self.vault, "01_Projects/Quorum/99 - nope.md")


class CliTest(Case):
    def setUp(self):
        super().setUp()
        self.env = dict(os.environ, QUORUM_VAULT=str(self.vault), QUORUM_OVERNIGHT_STORE=str(Path(self.tmp.name) / "store"),
                        QUORUM_OVERNIGHT_NOW=NOW.isoformat(), QUORUM_OVERNIGHT_FAKE="1")

    def cli(self, *args):
        return subprocess.run([sys.executable, str(HERE / "overnight.py")] + list(args), env=self.env,
                              capture_output=True, text=True)

    def test_help_and_no_project(self):
        self.assertEqual(self.cli("--help").returncode, 0)
        r = self.cli()
        self.assertEqual(r.returncode, 2)
        self.assertIn("quorum overnight <project>", r.stderr)

    def test_a_fake_night_through_the_cli_with_events(self):
        r = self.cli("permafrost", "--events")
        self.assertEqual(r.returncode, 0, r.stderr)
        evs = [json.loads(l) for l in r.stdout.splitlines() if l.strip()]
        self.assertEqual(evs[0]["event"], "started")
        self.assertEqual(evs[0]["project"], "permafrost")
        self.assertEqual(evs[-1]["event"], "done")
        self.assertEqual(evs[-1]["drafts"], 2)
        r = self.cli("list")
        self.assertIn("permafrost", r.stdout)
        self.assertIn("2026-10-29", r.stdout)
        r = self.cli("score")
        self.assertIn("permafrost", r.stdout)
        self.assertIn("2/4", r.stdout)

    def test_no_appendix_is_an_error_naming_the_heading(self):
        r = self.cli("meridian")
        self.assertEqual(r.returncode, 1)
        self.assertIn("## Project: meridian", r.stderr)


if __name__ == "__main__":
    unittest.main()
