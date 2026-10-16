#!/usr/bin/env python3
"""Tests for scripts/vault_index.py. Run: python3 scripts/vault_index_test.py"""
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import vault_index as vi  # noqa: E402


def note(path, body, **front):
    path.parent.mkdir(parents=True, exist_ok=True)
    fm = ""
    if front:
        fm = "---\n" + "".join("%s: %s\n" % kv for kv in front.items()) + "---\n\n"
    path.write_text(fm + body, encoding="utf-8")


class IndexCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.vault = root / "vault"
        self.mem = root / "memory"
        self.vault.mkdir()
        self.mem.mkdir()
        note(self.vault / "01_Projects" / "Quorum" / "15 - Briefing.md",
             "The rolling briefing. See [[06 - Decision Log]] and [[Spend Readout|the readout]].",
             title="Briefing", summary='"Where Quorum is"', tags="[quorum, briefing]")
        note(self.vault / "01_Projects" / "Quorum" / "06 - Decision Log.md",
             "Decision 95: dormant. Links back to [[15 - Briefing#State]].",
             title="Decision Log")
        note(self.vault / "03_Resources" / "Oracle Staleness.md",
             "Pyth Hermes prices go stale; 오라클 가격 지연.", title="Oracle Staleness")
        note(self.vault / ".obsidian" / "skip.md", "obsidian config oracle")
        note(self.vault / "node_modules" / "pkg" / "README.md", "oracle vendored")
        note(self.mem / "feedback_close_ritual.md",
             "Paste the spend line. Related: [[project_quorum_status]].",
             name="feedback-close-ritual",
             description='"Session close ritual: briefing + spend line"',
             metadata="")
        note(self.mem / "project_quorum_status.md", "Quorum is parked.",
             name="project_quorum_status", description='"Quorum status"')
        self.db = Path(self.tmp.name) / "index.db"
        self.idx = vi.Index(self.db, self.vault, self.mem)
        self.idx.refresh()

    def tearDown(self):
        self.idx.close()
        self.tmp.cleanup()

    def paths(self, hits):
        return [h["path"] for h in hits]

    # -- scan -------------------------------------------------------------
    def test_scan_counts_vault_and_memory_and_skips_excluded_dirs(self):
        st = self.idx.stats()
        self.assertEqual((st["vault"], st["memory"]), (3, 2))

    def test_frontmatter_title_summary_tags_are_read(self):
        d = self.idx.doc("vault:01_Projects/Quorum/15 - Briefing.md")
        self.assertEqual((d["title"], d["summary"], d["tags"]),
                         ("Briefing", "Where Quorum is", "quorum briefing"))

    def test_memory_description_is_its_summary(self):
        d = self.idx.doc("memory:feedback_close_ritual.md")
        self.assertEqual(d["summary"], "Session close ritual: briefing + spend line")

    def test_title_falls_back_to_the_file_name(self):
        note(self.vault / "No Front.md", "plain body")
        self.idx.refresh()
        self.assertEqual(self.idx.doc("vault:No Front.md")["title"], "No Front")

    # -- search -----------------------------------------------------------
    def test_search_finds_a_body_term(self):
        hits = self.idx.search("Hermes")
        self.assertEqual(self.paths(hits), ["vault:03_Resources/Oracle Staleness.md"])

    def test_search_ranks_a_title_match_above_a_body_match(self):
        note(self.vault / "Mentions.md", "the briefing is mentioned here once")
        self.idx.refresh()
        self.assertEqual(self.paths(self.idx.search("briefing"))[0],
                         "vault:01_Projects/Quorum/15 - Briefing.md")

    def test_search_matches_a_prefix(self):
        self.assertIn("vault:03_Resources/Oracle Staleness.md",
                      self.paths(self.idx.search("stal")))

    def test_search_finds_korean(self):
        self.assertEqual(self.paths(self.idx.search("오라클")),
                         ["vault:03_Resources/Oracle Staleness.md"])

    def test_search_survives_punctuation_and_fts_syntax(self):
        for q in ("what's `spend`?", 'AND OR NOT "', "a:b (c", "-x *"):
            self.idx.search(q)  # must not raise

    def test_search_any_term_matches_but_more_terms_rank_higher(self):
        hits = self.paths(self.idx.search("oracle briefing rolling"))
        self.assertEqual(hits[0], "vault:01_Projects/Quorum/15 - Briefing.md")
        self.assertIn("vault:03_Resources/Oracle Staleness.md", hits)

    def test_search_puts_an_archived_note_after_a_live_one(self):
        """Same terms, same strength: the live note first, the archived one after."""
        note(self.vault / "04_Archive" / "Old Lease.md",
             "walrus lease renewal; walrus lease renewal again", title="Old Lease")
        note(self.vault / "01_Projects" / "Live Lease.md", "walrus lease renewal", title="Old Lease")
        self.idx.refresh()
        self.assertEqual(self.paths(self.idx.search("walrus lease renewal")),
                         ["vault:01_Projects/Live Lease.md", "vault:04_Archive/Old Lease.md"])

    def test_search_can_filter_by_source(self):
        hits = self.idx.search("quorum", source="memory")
        self.assertTrue(hits and all(p.startswith("memory:") for p in self.paths(hits)))

    def test_search_never_returns_excluded_dirs(self):
        self.assertNotIn("vault:.obsidian/skip.md", self.paths(self.idx.search("oracle")))

    # -- links ------------------------------------------------------------
    def test_outgoing_links_resolve_by_file_name_dropping_alias_and_heading(self):
        out = self.idx.links("15 - Briefing")["outgoing"]
        self.assertIn(("06 - Decision Log", "vault:01_Projects/Quorum/06 - Decision Log.md"),
                      [(o["target"], o["path"]) for o in out])

    def test_an_unresolved_link_is_kept_with_no_path(self):
        out = self.idx.links("15 - Briefing")["outgoing"]
        self.assertIn(("Spend Readout", None), [(o["target"], o["path"]) for o in out])

    def test_backlinks_come_from_the_link_table(self):
        back = self.idx.links("06 - Decision Log")["backlinks"]
        self.assertEqual([b["path"] for b in back],
                         ["vault:01_Projects/Quorum/15 - Briefing.md"])

    def test_memory_links_resolve_to_memory_files(self):
        out = self.idx.links("feedback_close_ritual")["outgoing"]
        self.assertEqual([o["path"] for o in out], ["memory:project_quorum_status.md"])

    # -- freshness --------------------------------------------------------
    def test_a_changed_file_is_reindexed_on_refresh(self):
        p = self.vault / "03_Resources" / "Oracle Staleness.md"
        note(p, "now about RedStone feeds", title="Oracle Staleness")
        os.utime(p, (time.time() + 5, time.time() + 5))
        r = self.idx.refresh()
        self.assertEqual(r["changed"], 1)
        self.assertEqual(self.paths(self.idx.search("RedStone")),
                         ["vault:03_Resources/Oracle Staleness.md"])
        self.assertEqual(self.idx.search("Hermes"), [])

    def test_a_deleted_file_leaves_the_index_and_its_links(self):
        (self.vault / "01_Projects" / "Quorum" / "15 - Briefing.md").unlink()
        r = self.idx.refresh()
        self.assertEqual(r["removed"], 1)
        self.assertEqual(self.idx.links("06 - Decision Log")["backlinks"], [])

    def test_an_unchanged_tree_reindexes_nothing(self):
        r = self.idx.refresh()
        self.assertEqual((r["changed"], r["removed"]), (0, 0))

    def test_a_new_note_fills_a_link_that_was_unresolved(self):
        note(self.vault / "Spend Readout.md", "the readout")
        self.idx.refresh()
        out = self.idx.links("15 - Briefing")["outgoing"]
        self.assertIn(("Spend Readout", "vault:Spend Readout.md"),
                      [(o["target"], o["path"]) for o in out])


class CliCase(unittest.TestCase):
    """`quorum q` / `quorum links` / `quorum index` through scripts/quorum_cli.py."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.vault = root / "vault"
        self.mem = root / "mem"
        note(self.vault / "A Note.md", "walrus blob lease renewal", title="A Note")
        self.mem.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def cli(self, *args):
        env = dict(os.environ)
        env["QUORUM_VAULT"] = str(self.vault)
        env["QUORUM_MEMORY"] = str(self.mem)
        return subprocess.run([sys.executable, str(HERE / "quorum_cli.py")] + list(args),
                              env=env, capture_output=True, text=True)

    def test_q_prints_the_hit_and_builds_the_db_in_the_vault(self):
        p = self.cli("q", "lease")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("A Note.md", p.stdout)
        self.assertTrue((self.vault / ".quorum" / "index.db").exists())

    def test_q_json_is_a_list_of_hits(self):
        hits = json.loads(self.cli("q", "lease", "--json").stdout)
        self.assertEqual(hits[0]["path"], "vault:A Note.md")

    def test_q_without_terms_exits_2(self):
        self.assertEqual(self.cli("q").returncode, 2)

    def test_links_on_an_unknown_note_exits_1(self):
        self.assertEqual(self.cli("links", "Nope").returncode, 1)

    def test_index_stats(self):
        p = self.cli("index", "--stats")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("vault 1", p.stdout)


if __name__ == "__main__":
    unittest.main()
