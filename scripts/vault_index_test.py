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


def git(cwd, *args):
    subprocess.run(["git", "-C", str(cwd)] + list(args), check=True, capture_output=True,
                   env=dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
                            GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t"))


class MentionsCase(unittest.TestCase):
    """Step 2: notes -> the repos, files, commits and PRs they name."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.home = Path(self.tmp.name)
        self.old_home = os.environ.get("HOME")
        os.environ["HOME"] = str(self.home)
        # A repo with a commit, a worktree of it, a non-git dir, a parent dir
        # holding a repo one level down.
        self.repo = self.home / "nonvis" / "alpha"
        self.repo.mkdir(parents=True)
        git(self.repo, "init", "-q")
        git(self.repo, "remote", "add", "origin", "git@github.com:me/alpha.git")
        (self.repo / "scripts").mkdir()
        (self.repo / "scripts" / "run.py").write_text("print(1)\n")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-q", "-m", "c1")
        self.sha = subprocess.run(["git", "-C", str(self.repo), "rev-parse", "HEAD"],
                                  capture_output=True, text=True).stdout.strip()
        git(self.repo, "worktree", "add", "-q", str(self.home / "nonvis" / "alpha-wt"))
        (self.home / "nonvis" / "datadir").mkdir()
        inner = self.home / "projects" / "fluid" / "fluid-pkgs"
        inner.mkdir(parents=True)
        git(inner, "init", "-q")
        self.vault = self.home / "vault"
        note(self.vault / "A.md",
             "Ran `~/nonvis/alpha/scripts/run.py` at `%s`; see PR #12." % self.sha[:8],
             title="A")
        note(self.vault / "B.md",
             "The worktree ~/nonvis/alpha-wt/scripts/run.py and ~/nonvis/datadir/x and "
             "~/nonvis/gone/y and ~/projects/fluid/fluid-pkgs/README.md. Decision #98.",
             title="B")
        note(self.vault / "C.md", "Edit `scripts/run.py` only.", title="C")
        (self.vault / "06_Metadata").mkdir()
        (self.vault / "06_Metadata" / "quorum-repos.txt").write_text(
            "# repos the index may read\nalpha\n")
        self.idx = vi.Index(self.home / "index.db", self.vault, None)
        self.idx.refresh()

    def tearDown(self):
        self.idx.close()
        if self.old_home is None:
            del os.environ["HOME"]
        else:
            os.environ["HOME"] = self.old_home
        self.tmp.cleanup()

    def repos(self):
        return {r["name"]: r for r in self.idx.repos()}

    def test_a_worktree_folds_into_its_repo(self):
        r = self.repos()["alpha"]
        self.assertEqual((r["status"], r["mentions"], r["notes"]), ("repo", 2, 2))

    def test_a_repo_records_its_remote_and_the_allowlist(self):
        r = self.repos()["alpha"]
        self.assertEqual((r["remote"], r["allowed"]), ("git@github.com:me/alpha.git", True))

    def test_a_repo_one_level_under_a_parent_dir_is_found(self):
        r = self.repos()["fluid-pkgs"]
        self.assertEqual((r["status"], r["allowed"]), ("repo", False))

    def test_absent_and_non_git_roots_are_recorded_not_dropped(self):
        st = {r["name"]: r["status"] for r in self.idx.repos()}
        self.assertEqual((st["gone"], st["datadir"]), ("absent", "not-git"))

    def test_mentions_of_a_repo_list_its_notes(self):
        self.assertEqual([m["path"] for m in self.idx.mentions("alpha")],
                         ["vault:A.md", "vault:B.md"])

    def test_mentions_of_a_file_match_full_and_bare_relative_paths(self):
        self.assertEqual([m["path"] for m in self.idx.mentions("scripts/run.py")],
                         ["vault:A.md", "vault:B.md", "vault:C.md"])

    def test_mentions_of_a_bare_file_name_match_full_paths_too(self):
        self.assertEqual([m["path"] for m in self.idx.mentions("run.py")],
                         ["vault:A.md", "vault:B.md", "vault:C.md"])

    def test_mentions_of_a_commit_match_by_prefix_and_name_the_repo(self):
        hits = self.idx.mentions(self.sha)
        self.assertEqual([(m["path"], m["repo"]) for m in hits], [("vault:A.md", "alpha")])

    def test_a_pr_is_captured_but_a_bare_hash_number_is_not(self):
        kinds = sorted((m["kind"], m["value"]) for m in self.idx.note_mentions("A"))
        self.assertIn(("pr", "12"), kinds)
        self.assertNotIn("98", [v for _, v in sorted(
            (m["kind"], m["value"]) for m in self.idx.note_mentions("B"))])

    def test_a_note_lists_what_it_mentions(self):
        kinds = sorted({m["kind"] for m in self.idx.note_mentions("A")})
        self.assertEqual(kinds, ["path", "pr", "sha"])

    def test_an_edited_note_replaces_its_mentions(self):
        note(self.vault / "A.md", "nothing now", title="A")
        os.utime(self.vault / "A.md", (time.time() + 5, time.time() + 5))
        self.idx.refresh()
        self.assertEqual([m["path"] for m in self.idx.mentions("alpha")], ["vault:B.md"])


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

    def test_repos_and_mentions_commands(self):
        r = self.cli("repos")
        self.assertEqual(r.returncode, 0, r.stderr)
        m = self.cli("mentions", "nothing-mentions-this")
        self.assertEqual(m.returncode, 0, m.stderr)

    def test_index_stats(self):
        p = self.cli("index", "--stats")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("vault 1", p.stdout)


if __name__ == "__main__":
    unittest.main()
