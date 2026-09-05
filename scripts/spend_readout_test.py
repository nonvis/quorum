#!/usr/bin/env python3
"""
Deterministic gate for spend_readout.py (stdlib `unittest` only, no deps).

The property under test is ABSENT != ZERO: a transcript source the readout
cannot see must never be reported as `$0.00`. Each case drives the REAL CLI as a
subprocess with HOME pointed at a throwaway dir, so nothing here reads the
operator's `~/.claude` — HOME selects both the transcript root and the
settings.json that supplies `cleanupPeriodDays`.

ONE assertion per test method — unittest stops a method at its first failure, so
bundled assertions cannot be owned separately and a mutation would red them as a
block. Case map:
  absent dir      -> no "$0.00" anywhere · exit 3 · the ABSENT header wording ·
                     JSON status no_transcript_dir · JSON total.est_usd null
  empty dir       -> "$0.00" · exit 0 · "sessions scanned: 0"  — the LIVENESS
                     case the absent-refusal must NOT fire on
  unknown family  -> fable still priced · unknown est null · named in JSON ·
                     JSON total flagged a floor · human "TOTAL is a FLOOR" ·
                     the unrated row still shows its tokens
  --since old     -> the retention WARNING line
  --since recent  -> NO warning line          (liveness for the warning)
  settings.json   -> horizon + source read from cleanupPeriodDays, not the 30d
                     default; and the default when the key is absent
  bad --since     -> exit 2, distinct from 3

Run manually: python3 scripts/spend_readout_test.py   (exit 0 = pass)
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
SCRIPT = HERE / "spend_readout.py"

PROJECT = "/tmp/quorum-spend-gate/proj"


def munge(abs_path):
    """The script's own path rule — kept independent so a drift in it reds."""
    return "".join("-" if c in "/." else c for c in abs_path)


def iso(dt):
    return dt.strftime("%Y-%m-%dT%H:%M:%SZ")


def assistant_record(model, ts, tokens=1000):
    return json.dumps({
        "type": "assistant",
        "timestamp": iso(ts),
        "message": {
            "id": "msg_%s" % model,
            "model": model,
            "usage": {
                "input_tokens": tokens,
                "output_tokens": tokens,
                "cache_creation_input_tokens": 0,
                "cache_read_input_tokens": 0,
            },
        },
    })


class SpendReadoutCase(unittest.TestCase):

    def setUp(self):
        self.home = Path(tempfile.mkdtemp(prefix="spend_gate_home_"))
        (self.home / ".claude" / "projects").mkdir(parents=True)
        self.now = datetime.now(timezone.utc).replace(tzinfo=None, microsecond=0)

    def tearDown(self):
        shutil.rmtree(self.home, ignore_errors=True)

    # -- helpers ---------------------------------------------------------

    def tdir(self):
        return self.home / ".claude" / "projects" / munge(PROJECT)

    def write_settings(self, obj):
        (self.home / ".claude" / "settings.json").write_text(json.dumps(obj))

    def run_readout(self, since, extra=()):
        env = dict(os.environ)
        env["HOME"] = str(self.home)
        p = subprocess.run(
            [sys.executable, str(SCRIPT), "--project", PROJECT,
             "--since", since] + list(extra),
            env=env, capture_output=True, text=True)
        return p

    # -- 1. absent dir: the refusal --------------------------------------
    # One assertion per method: a mutation stops at the first failure in a
    # method, so assertions bundled together cannot be owned separately.

    def test_absent_dir_never_prints_a_dollar_zero(self):
        """The headline property: a source we cannot see is not a $0 spend."""
        self.assertFalse(self.tdir().exists())
        p = self.run_readout(iso(self.now - timedelta(hours=1)))
        self.assertNotIn("$0.00", p.stdout,
                         "absent != zero: the readout printed a $0.00\n" + p.stdout)

    def test_absent_dir_exits_3(self):
        p = self.run_readout(iso(self.now - timedelta(hours=1)))
        self.assertEqual(p.returncode, 3,
                         "absent transcript dir must exit 3 (0=measured, "
                         "2=bad args)\n" + p.stdout)

    def test_absent_dir_header_names_the_reason(self):
        p = self.run_readout(iso(self.now - timedelta(hours=1)))
        self.assertIn(
            "transcripts: %s — ABSENT (never a session with this cwd, or "
            "pruned by Claude Code's transcript retention)" % self.tdir(),
            p.stdout)

    def test_absent_dir_json_status(self):
        p = self.run_readout(iso(self.now - timedelta(hours=1)), ["--json"])
        self.assertEqual(json.loads(p.stdout)["status"], "no_transcript_dir")

    def test_absent_dir_json_total_is_null(self):
        p = self.run_readout(iso(self.now - timedelta(hours=1)), ["--json"])
        self.assertIsNone(json.loads(p.stdout)["total"]["est_usd"])

    # -- 2. empty dir: the LIVENESS case the refusal must not fire on -----

    def test_empty_dir_prints_a_real_zero(self):
        """LIVENESS: dir present, nothing in it => $0.00. The refusal must NOT
        fire one step the other side of the boundary it guards."""
        self.tdir().mkdir(parents=True)
        p = self.run_readout(iso(self.now - timedelta(hours=1)))
        self.assertIn("$0.00", p.stdout,
                      "an empty (but PRESENT) dir IS zero\n" + p.stdout)

    def test_empty_dir_exits_0(self):
        self.tdir().mkdir(parents=True)
        p = self.run_readout(iso(self.now - timedelta(hours=1)))
        self.assertEqual(p.returncode, 0, p.stdout)

    def test_empty_dir_scans_zero_sessions(self):
        self.tdir().mkdir(parents=True)
        p = self.run_readout(iso(self.now - timedelta(hours=1)))
        self.assertIn("sessions scanned: 0", p.stdout)

    # -- 3. an unknown model family is n/a, and floors the TOTAL ----------

    def seed_two_families(self):
        d = self.tdir()
        d.mkdir(parents=True)
        ts = self.now - timedelta(minutes=1)
        (d / "s1.jsonl").write_text(
            assistant_record("claude-fable-5-1", ts) + "\n" +
            assistant_record("some-future-model-9", ts) + "\n")
        return d

    def json_two_families(self):
        self.seed_two_families()
        p = self.run_readout(iso(self.now - timedelta(hours=1)), ["--json"])
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        return json.loads(p.stdout)

    def test_known_family_still_priced(self):
        r = self.json_two_families()
        by_model = {m["model"]: m for m in r["models"]}
        # 1000 in @ $10/MTok + 1000 out @ $50/MTok = $0.06
        self.assertAlmostEqual(by_model["claude-fable-5-1"]["est_usd"], 0.06, 4)

    def test_unknown_family_json_est_is_null(self):
        r = self.json_two_families()
        by_model = {m["model"]: m for m in r["models"]}
        self.assertIsNone(by_model["some-future-model-9"]["est_usd"],
                          "an unrated family must be n/a, not $0")

    def test_unknown_family_is_named_in_json(self):
        self.assertEqual(self.json_two_families()["unknown_families"],
                         ["some-future-model-9"])

    def test_unknown_family_makes_json_total_a_floor(self):
        self.assertTrue(self.json_two_families()["total"]["est_is_floor"])

    def test_unknown_family_human_total_says_floor(self):
        self.seed_two_families()
        p = self.run_readout(iso(self.now - timedelta(hours=1)))
        self.assertIn("TOTAL is a FLOOR", p.stdout)

    def test_unknown_family_human_row_shows_tokens_and_na(self):
        """Absent rate != absent usage: the tokens are still on the row."""
        self.seed_two_families()
        p = self.run_readout(iso(self.now - timedelta(hours=1)))
        self.assertRegex(p.stdout, r"some-future-model-9\s+1,000\s+1,000.*n/a")

    # -- 4/5. the retention horizon warning, and its liveness -------------

    def test_since_before_horizon_warns(self):
        self.tdir().mkdir(parents=True)
        p = self.run_readout(iso(self.now - timedelta(days=60)))
        self.assertIn("may have been pruned — totals are a FLOOR", p.stdout)

    def test_since_after_horizon_does_not_warn(self):
        """The warning's liveness partner: one step inside the horizon, silent."""
        self.tdir().mkdir(parents=True)
        p = self.run_readout(iso(self.now - timedelta(days=1)))
        self.assertNotIn("may have been pruned", p.stdout)

    def test_horizon_line_always_prints(self):
        self.tdir().mkdir(parents=True)
        p = self.run_readout(iso(self.now - timedelta(days=1)))
        self.assertIn("retention horizon", p.stdout)

    def test_horizon_reads_settings_json(self):
        self.write_settings({"cleanupPeriodDays": 7})
        self.tdir().mkdir(parents=True)
        expected = (self.now - timedelta(days=7)).date().isoformat()
        p = self.run_readout(iso(self.now - timedelta(days=1)))
        self.assertIn("retention horizon ≈ %s" % expected, p.stdout)

    def test_horizon_names_settings_json_as_the_source(self):
        self.write_settings({"cleanupPeriodDays": 7})
        self.tdir().mkdir(parents=True)
        p = self.run_readout(iso(self.now - timedelta(days=1)))
        self.assertIn("source: settings.json", p.stdout)

    def test_horizon_falls_back_to_default_when_unset(self):
        """settings.json exists but sets no cleanupPeriodDays => 30, "default"."""
        self.write_settings({"model": "claude-fable-5-1[1m]"})
        self.tdir().mkdir(parents=True)
        expected = (self.now - timedelta(days=30)).date().isoformat()
        p = self.run_readout(iso(self.now - timedelta(days=1)))
        self.assertIn("retention horizon ≈ %s" % expected, p.stdout)

    def test_horizon_names_default_as_the_source(self):
        self.write_settings({"model": "claude-fable-5-1[1m]"})
        self.tdir().mkdir(parents=True)
        p = self.run_readout(iso(self.now - timedelta(days=1)))
        self.assertIn("source: default", p.stdout)

    def test_nonpositive_cleanup_period_falls_back(self):
        """A 0/negative setting must not become a zero-day horizon (which would
        warn on every window). Fall back rather than assert nonsense."""
        self.write_settings({"cleanupPeriodDays": 0})
        self.tdir().mkdir(parents=True)
        expected = (self.now - timedelta(days=30)).date().isoformat()
        p = self.run_readout(iso(self.now - timedelta(days=1)))
        self.assertIn("retention horizon ≈ %s  (cleanupPeriodDays=30, "
                      "source: default)" % expected, p.stdout)

    def test_unreadable_settings_falls_back(self):
        """A corrupt settings.json must not silently widen the horizon."""
        (self.home / ".claude" / "settings.json").write_text("{ not json")
        self.tdir().mkdir(parents=True)
        expected = (self.now - timedelta(days=30)).date().isoformat()
        p = self.run_readout(iso(self.now - timedelta(days=1)))
        self.assertIn("retention horizon ≈ %s  (cleanupPeriodDays=30, "
                      "source: default)" % expected, p.stdout)

    # -- 6. the other two exit codes stay distinct ------------------------

    def test_bad_since_exits_2(self):
        """2 (bad args) must not collide with 3 (absent source)."""
        p = self.run_readout("not-a-timestamp")
        self.assertEqual(p.returncode, 2, p.stdout + p.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
