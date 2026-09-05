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
  absent + db     -> the daemon ledger IS read and labelled the only reading
                     (sum + conversation count, out-of-window rows excluded),
                     and the TOTAL still says n/a — one readable source does not
                     become the missing one's number (Decision #64)
  absent, no db   -> "db cross-check: unavailable (no .quorum/quorum.db)"
  empty dir       -> "$0.00" · exit 0 · "sessions scanned: 0"  — the LIVENESS
                     case the absent-refusal must NOT fire on
  exact rates     -> sonnet-5 $2/$10 vs sonnet-4-6 $3/$15 (the substring table
                     mispriced the first as the second) · a dated id resolves to
                     its base row · fable 1h cache creation bills 2x and 5m
                     1.25x · fable cache READ is flat $0.25/MTok while a
                     non-exempt model reads at 0.1x input · an untiered record
                     prices at 5m AND says so · an unlisted id that still
                     matches a family is priced at the family rate and FLAGGED
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
import sqlite3
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
    return usage_record(model, ts, input_tokens=tokens, output_tokens=tokens)


def usage_record(model, ts, **usage):
    """One assistant record with an arbitrary `usage` object.

    Defaults every counter to 0 so a case can name ONLY the field it prices —
    a rate test that silently carried 1000 input tokens would not isolate the
    multiplier it claims to measure.
    """
    u = {"input_tokens": 0, "output_tokens": 0,
         "cache_creation_input_tokens": 0, "cache_read_input_tokens": 0}
    u.update(usage)
    return json.dumps({
        "type": "assistant",
        "timestamp": iso(ts),
        "message": {"id": "msg_%s" % model, "model": model, "usage": u},
    })


class SpendReadoutCase(unittest.TestCase):

    def setUp(self):
        self.home = Path(tempfile.mkdtemp(prefix="spend_gate_home_"))
        (self.home / ".claude" / "projects").mkdir(parents=True)
        # A REAL project dir, for the cases that need a .quorum/quorum.db on
        # disk. Its transcript dir is still absent unless a case makes it.
        self.proj = Path(tempfile.mkdtemp(prefix="spend_gate_proj_"))
        (self.proj / ".quorum").mkdir()
        self.now = datetime.now(timezone.utc).replace(tzinfo=None, microsecond=0)

    def tearDown(self):
        shutil.rmtree(self.home, ignore_errors=True)
        shutil.rmtree(self.proj, ignore_errors=True)

    # -- helpers ---------------------------------------------------------

    def tdir(self):
        return self.home / ".claude" / "projects" / munge(PROJECT)

    def write_settings(self, obj):
        (self.home / ".claude" / "settings.json").write_text(json.dumps(obj))

    def run_readout(self, since, extra=(), project=PROJECT):
        env = dict(os.environ)
        env["HOME"] = str(self.home)
        p = subprocess.run(
            [sys.executable, str(SCRIPT), "--project", str(project),
             "--since", since] + list(extra),
            env=env, capture_output=True, text=True)
        return p

    def seed_db(self, rows):
        """Write a minimal conversations table — the columns the script reads.

        Deliberately NOT the daemon's full schema: the cross-check must depend
        only on (spent_usd, created_at), and a drift into reading more would red
        here rather than at `quorum spend`.
        """
        db = self.proj / ".quorum" / "quorum.db"
        con = sqlite3.connect(str(db))
        con.execute("CREATE TABLE conversations (id INTEGER PRIMARY KEY, "
                    "goal TEXT, spent_usd REAL, created_at TEXT)")
        con.executemany(
            "INSERT INTO conversations(goal, spent_usd, created_at) "
            "VALUES(?,?,?)", rows)
        con.commit()
        con.close()
        return db

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

    # -- 1b. absent dir, but the daemon ledger is still a real reading ----
    # The transcript source is unreadable; the db is a DIFFERENT source and is
    # readable. Refusing to look at it too would throw away the only measurement
    # left — but it also must not be promoted into the TOTAL (Decision #64: the
    # daemon only records `converse`, so it is a partial view of the window).

    def db_window(self):
        return (iso(self.now - timedelta(hours=3)), iso(self.now))

    def seed_window_db(self):
        def at(delta):
            return (self.now - delta).strftime("%Y-%m-%d %H:%M:%S")
        return self.seed_db([
            ("in-window a", 1.25, at(timedelta(hours=2))),
            ("in-window b", 2.50, at(timedelta(hours=1))),
            ("OUTSIDE the window", 99.0, at(timedelta(hours=30))),
        ])

    def run_absent_with_db(self, extra=()):
        self.seed_window_db()
        since, until = self.db_window()
        p = self.run_readout(since, ["--until", until] + list(extra),
                             project=self.proj)
        self.assertEqual(p.returncode, 3, p.stdout + p.stderr)
        return p

    def test_absent_dir_runs_the_db_cross_check(self):
        """The line the operator reads: labelled, summed, counted."""
        p = self.run_absent_with_db()
        self.assertIn("db cross-check (daemon-recorded, the only reading for "
                      "this window): $3.75 across 2 conversations", p.stdout)

    def test_absent_dir_db_cross_check_excludes_out_of_window(self):
        """The $99 row sits outside [since, until] and must not be counted."""
        r = json.loads(self.run_absent_with_db(["--json"]).stdout)
        self.assertEqual(r["db_cross_check_conversations"], 2)

    def test_absent_dir_db_cross_check_json_carries_the_sum(self):
        r = json.loads(self.run_absent_with_db(["--json"]).stdout)
        self.assertAlmostEqual(r["db_cross_check_usd"], 3.75, places=6)

    def test_absent_dir_db_figure_never_becomes_the_total(self):
        """Decision #64: a readable second source is NOT the missing first one.
        The TOTAL row stays n/a even though we now print a real dollar figure."""
        p = self.run_absent_with_db()
        self.assertRegex(p.stdout, r"TOTAL\s+n/a\s+n/a\s+n/a\s+n/a\s+n/a")

    def test_absent_dir_db_figure_never_becomes_the_json_total(self):
        r = json.loads(self.run_absent_with_db(["--json"]).stdout)
        self.assertIsNone(r["total"]["est_usd"])

    def test_absent_dir_without_a_db_says_unavailable(self):
        """No transcripts AND no ledger: name the reason, claim no number."""
        since, until = self.db_window()
        p = self.run_readout(since, ["--until", until], project=self.proj)
        self.assertIn("db cross-check: unavailable (no .quorum/quorum.db)",
                      p.stdout)

    def test_absent_dir_without_a_db_prints_no_dollar_zero(self):
        """`unavailable` must not degrade into a $0.00 anywhere on the page."""
        since, until = self.db_window()
        p = self.run_readout(since, ["--until", until], project=self.proj)
        self.assertNotIn("$0.00", p.stdout, p.stdout)

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

    # -- 3b. EXACT-id rates, cache tiers, and the flagged family fallback --
    # Source of the numbers under test: claude-api skill (Claude Code 2.1.261
    # bundle), pricing table cached 2026-06-24.

    def seed_one(self, model, **usage):
        d = self.tdir()
        d.mkdir(parents=True, exist_ok=True)
        (d / "s1.jsonl").write_text(
            usage_record(model, self.now - timedelta(minutes=1), **usage) + "\n")

    def price_of(self, model, **usage):
        """EST $ for a single record of `model` with exactly this usage."""
        self.seed_one(model, **usage)
        p = self.run_readout(iso(self.now - timedelta(hours=1)), ["--json"])
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        rows = {m["model"]: m for m in json.loads(p.stdout)["models"]}
        return rows[model]["est_usd"]

    def test_sonnet_5_prices_at_its_own_rate(self):
        """$2/$10 — the substring table charged it Sonnet 4.6's $3/$15."""
        self.assertAlmostEqual(
            self.price_of("claude-sonnet-5", input_tokens=1000,
                          output_tokens=1000), 0.012, places=6)

    def test_sonnet_4_6_keeps_the_older_rate(self):
        """The discriminator: same family, different price. $3/$15."""
        self.assertAlmostEqual(
            self.price_of("claude-sonnet-4-6", input_tokens=1000,
                          output_tokens=1000), 0.018, places=6)

    def test_dated_model_id_resolves_to_its_base_row(self):
        """Longest-prefix: a dated/suffixed id is not an unknown model."""
        self.assertAlmostEqual(
            self.price_of("claude-sonnet-5-20260101", input_tokens=1000,
                          output_tokens=1000), 0.012, places=6)

    def test_bracket_suffix_id_still_reads_at_the_flat_rate(self):
        """The FLAT-read table matches by prefix too: `claude-fable-5-1[1m]`
        reads at $0.25/MTok, not 0.1x its input rate."""
        self.assertAlmostEqual(
            self.price_of("claude-fable-5-1[1m]",
                          cache_read_input_tokens=4000), 0.001, places=6)

    def test_bracket_suffix_id_resolves_as_an_exact_row(self):
        """…and it is EXACT, not a family guess — the price happens to match the
        family rate, so only rate_source can tell prefix resolution worked."""
        self.seed_one("claude-fable-5-1[1m]", input_tokens=1000)
        p = self.run_readout(iso(self.now - timedelta(hours=1)), ["--json"])
        rows = {m["model"]: m for m in json.loads(p.stdout)["models"]}
        self.assertEqual(rows["claude-fable-5-1[1m]"]["rate_source"], "exact")

    def test_one_hour_cache_creation_bills_double(self):
        """1h tier = 2x input. fable $10 => $20/MTok on 1000 tok = $0.02."""
        self.assertAlmostEqual(
            self.price_of("claude-fable-5-1",
                          cache_creation={"ephemeral_5m_input_tokens": 0,
                                          "ephemeral_1h_input_tokens": 1000}),
            0.02, places=6)

    def test_five_minute_cache_creation_bills_1_25x(self):
        """The tier's partner case: same tokens, 1.25x => $0.0125."""
        self.assertAlmostEqual(
            self.price_of("claude-fable-5-1",
                          cache_creation={"ephemeral_5m_input_tokens": 1000,
                                          "ephemeral_1h_input_tokens": 0}),
            0.0125, places=6)

    def test_fable_5_1_cache_read_is_a_flat_rate(self):
        """$0.25/MTok flat, NOT 0.1x$10=$1. 4000 tok => $0.001, not $0.004."""
        self.assertAlmostEqual(
            self.price_of("claude-fable-5-1", cache_read_input_tokens=4000),
            0.001, places=6)

    def test_non_exempt_cache_read_is_ten_percent_of_input(self):
        """The flat rate's liveness partner: sonnet-5 reads at 0.1x$2=$0.20."""
        self.assertAlmostEqual(
            self.price_of("claude-sonnet-5", cache_read_input_tokens=4000),
            0.0008, places=6)

    def test_untiered_cache_creation_prices_at_the_5m_rate(self):
        """An older record with no TTL split: 1.25x, not 2x. 1000 tok @ $2."""
        self.assertAlmostEqual(
            self.price_of("claude-sonnet-5",
                          cache_creation_input_tokens=1000), 0.0025, places=6)

    def test_untiered_cache_creation_says_it_assumed_5m(self):
        """An assumption the reader cannot see is an assumption they'll quote."""
        self.seed_one("claude-sonnet-5", cache_creation_input_tokens=1000)
        p = self.run_readout(iso(self.now - timedelta(hours=1)))
        self.assertIn("cache-creation TTL split absent for claude-sonnet-5",
                      p.stdout)

    def test_tiered_record_does_not_claim_an_assumption(self):
        """Liveness for that warning: a split record must NOT print it."""
        self.seed_one("claude-sonnet-5",
                      cache_creation={"ephemeral_5m_input_tokens": 1000,
                                      "ephemeral_1h_input_tokens": 0})
        p = self.run_readout(iso(self.now - timedelta(hours=1)))
        self.assertNotIn("cache-creation TTL split absent", p.stdout)

    def test_unlisted_id_falls_back_to_the_family_rate(self):
        """No row for claude-opus-9, but 'opus' is known: $5/$25, not n/a."""
        self.assertAlmostEqual(
            self.price_of("claude-opus-9", input_tokens=1000,
                          output_tokens=1000), 0.03, places=6)

    def test_family_fallback_is_flagged_in_json(self):
        self.seed_one("claude-opus-9", input_tokens=1000, output_tokens=1000)
        p = self.run_readout(iso(self.now - timedelta(hours=1)), ["--json"])
        rows = {m["model"]: m for m in json.loads(p.stdout)["models"]}
        self.assertEqual(rows["claude-opus-9"]["rate_source"], "family")

    def test_family_fallback_is_flagged_in_the_human_readout(self):
        """A guessed rate has to announce itself where the operator reads it."""
        self.seed_one("claude-opus-9", input_tokens=1000, output_tokens=1000)
        p = self.run_readout(iso(self.now - timedelta(hours=1)))
        self.assertIn("family-rate: claude-opus-9 has no row in the priced "
                      "table", p.stdout)

    def test_exact_id_is_not_flagged_as_a_family_rate(self):
        """Liveness for the flag: a priced id must not wear the warning."""
        self.seed_one("claude-sonnet-5", input_tokens=1000, output_tokens=1000)
        p = self.run_readout(iso(self.now - timedelta(hours=1)))
        self.assertNotIn("family-rate:", p.stdout)

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
