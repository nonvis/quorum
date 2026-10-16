#!/usr/bin/env python3
"""Tests for scripts/repo_index.py (step 3). Run: python3 scripts/repo_index_test.py"""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import repo_index as ri  # noqa: E402

ENV = dict(os.environ, GIT_AUTHOR_NAME="Ada", GIT_AUTHOR_EMAIL="a@a",
           GIT_COMMITTER_NAME="Ada", GIT_COMMITTER_EMAIL="a@a")


def git(cwd, *args):
    return subprocess.run(["git", "-C", str(cwd)] + list(args), check=True,
                          capture_output=True, text=True, env=ENV).stdout.strip()


def write(root, rel, text):
    p = Path(root) / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)


class RepoIndexCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name) / "alpha"
        self.repo.mkdir()
        git(self.repo, "init", "-q", "-b", "main")
        write(self.repo, "README.md", "# Alpha\n\nThe lease renewal service.\n")
        write(self.repo, "docs/design.md", "---\ntitle: Design\n---\nWalrus blob epochs.\n")
        write(self.repo, "src/lease.py", "class LeaseBook:\n    def renew(self):\n        pass\n\n"
                                         "def extend_lease(x):\n    return x\n")
        write(self.repo, "src/vault.rs", "pub struct Vault {}\npub fn open_vault() {}\n"
                                         "impl Vault {\n    fn seal(&self) {}\n}\nenum Mode { A }\n")
        write(self.repo, "web/app.ts", "export function renderRoom() {}\n"
                                       "export class RoomShell {}\nexport interface Props {}\n"
                                       "export const MAX_ROOMS = 3;\ntype Id = string;\n")
        write(self.repo, "move/sources/pool.move", "module alpha::pool {\n"
                                                   "    public struct Pool has key { id: UID }\n"
                                                   "    public fun deposit(p: &mut Pool) {}\n"
                                                   "    const E_EMPTY: u64 = 1;\n}\n")
        write(self.repo, "core/engine.cpp", "namespace eng {\nclass OrderBook {\n};\n"
                                            "int match_orders(int a) {\n  return a;\n}\n}\n")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-q", "-m", "lease: first cut",
            "-m", "Why: the renewal used to run nightly; now it is in place.")
        self.idx = ri.RepoIndex(self.repo)
        self.first = self.idx.refresh()

    def tearDown(self):
        self.idx.close()
        self.tmp.cleanup()

    def commit(self, msg):
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", msg)

    # -- storage + hygiene --------------------------------------------------
    def test_the_db_lives_in_the_repo_and_the_repo_stays_clean(self):
        self.assertTrue((self.repo / ".quorum" / "index.db").exists())
        self.assertEqual(git(self.repo, "status", "--porcelain"), "")

    def test_an_existing_quorum_gitignore_is_left_alone(self):
        self.assertEqual((self.repo / ".quorum" / ".gitignore").read_text(), "*\n")
        (self.repo / ".quorum" / ".gitignore").write_text("*\n# mine\n")
        ri.RepoIndex(self.repo).refresh()
        self.assertEqual((self.repo / ".quorum" / ".gitignore").read_text(), "*\n# mine\n")

    def test_the_first_refresh_indexes_every_tracked_file_and_commit(self):
        self.assertEqual((self.first["files"], self.first["commits"]), (7, 1))

    # -- symbols ------------------------------------------------------------
    def sym(self, name):
        return [(s["name"], s["kind"], s["path"]) for s in self.idx.symbol(name)]

    def test_python_rust_ts_move_cpp_definitions(self):
        found = {s["name"] for s in self.idx.symbol("")}
        for n in ("LeaseBook", "renew", "extend_lease", "Vault", "open_vault", "seal", "Mode",
                  "renderRoom", "RoomShell", "Props", "MAX_ROOMS", "Id", "pool", "Pool",
                  "deposit", "E_EMPTY", "eng", "OrderBook", "match_orders"):
            self.assertIn(n, found)

    def test_vendored_code_is_listed_but_not_parsed(self):
        write(self.repo, "third_party/sqlite/sqlite3.h", "int sqlite3_open(const char *f) {\n}\n")
        write(self.repo, "vendor/lib/README.md", "# vendored zebra docs\n")
        self.commit("vendor")
        self.idx.refresh()
        self.assertEqual((self.idx.symbol("sqlite3_open"), self.idx.search("zebra")), ([], []))

    def test_a_symbol_lookup_gives_path_and_line(self):
        s = self.idx.symbol("extend_lease")[0]
        self.assertEqual((s["path"], s["line"], s["kind"]), ("src/lease.py", 5, "def"))

    def test_symbol_lookup_is_a_case_insensitive_prefix(self):
        self.assertEqual({n for n, _, _ in self.sym("lease")}, {"LeaseBook"})

    # -- search ---------------------------------------------------------------
    def kinds(self, q):
        return [(h["kind"], h["ref"]) for h in self.idx.search(q)]

    def test_search_finds_markdown_by_body(self):
        self.assertIn(("doc", "docs/design.md"), self.kinds("epochs"))

    def test_docs_and_root_guides_rank_above_other_markdown(self):
        """docs/ and README/ARCHITECTURE/OPERATOR/CLAUDE are the repo's locked
        record; a fixture or changelog naming the same words ranks after them."""
        write(self.repo, "tests/fixtures/sample.md", "walrus blob epochs walrus blob epochs\n")
        self.commit("fixture")
        self.idx.refresh()
        refs = [h["ref"] for h in self.idx.search("walrus blob epochs") if h["kind"] == "doc"]
        self.assertEqual(refs[0], "docs/design.md")
        self.assertTrue(self.idx.search("walrus blob epochs")[0]["docs"])

    def test_search_finds_a_commit_by_its_message_body(self):
        hits = self.idx.search("nightly")
        self.assertEqual([h["kind"] for h in hits], ["commit"])
        self.assertEqual(hits[0]["title"], "lease: first cut")

    def test_search_finds_a_symbol_by_name(self):
        self.assertIn(("symbol", "web/app.ts:2"), self.kinds("RoomShell"))

    # -- freshness ------------------------------------------------------------
    def test_an_unchanged_head_reindexes_nothing(self):
        r = self.idx.refresh()
        self.assertEqual((r["files"], r["commits"]), (0, 0))

    def test_a_new_commit_reindexes_only_its_changed_files(self):
        write(self.repo, "src/lease.py", "def shorten_lease(x):\n    return x\n")
        self.commit("lease: shorten")
        r = self.idx.refresh()
        self.assertEqual((r["files"], r["commits"]), (1, 1))
        self.assertEqual(self.sym("extend_lease"), [])
        self.assertEqual(self.sym("shorten_lease"), [("shorten_lease", "def", "src/lease.py")])

    def test_a_deleted_file_leaves_the_index(self):
        (self.repo / "docs" / "design.md").unlink()
        self.commit("drop design")
        self.idx.refresh()
        self.assertNotIn(("doc", "docs/design.md"), self.kinds("epochs"))

    def test_a_rewritten_history_rebuilds_the_commits(self):
        write(self.repo, "x.md", "temporary\n")
        self.commit("temp commit about zebras")
        self.idx.refresh()
        git(self.repo, "reset", "-q", "--hard", "HEAD~1")
        self.idx.refresh()
        self.assertEqual(self.idx.search("zebras"), [])
        self.assertEqual(self.idx.stats()["commits"], 1)

    def test_uncommitted_edits_are_not_indexed(self):
        write(self.repo, "README.md", "# Alpha\n\nquokka\n")
        self.idx.refresh()
        self.assertEqual(self.idx.search("quokka"), [])


class OptInCase(unittest.TestCase):
    """A repo is indexed only if allowlisted (vault-driven) or opted in (.quorum/)."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name) / "beta"
        self.repo.mkdir()
        git(self.repo, "init", "-q")
        write(self.repo, "a.md", "hello\n")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-q", "-m", "c")

    def tearDown(self):
        self.tmp.cleanup()

    def cli(self, *args, cwd=None):
        env = dict(ENV, QUORUM_VAULT=str(Path(self.tmp.name) / "no-vault"))
        return subprocess.run([sys.executable, str(HERE / "quorum_cli.py")] + list(args),
                              cwd=str(cwd or self.repo), env=env, capture_output=True, text=True)

    def test_a_repo_without_quorum_dir_is_refused_from_inside(self):
        p = self.cli("repo", "q", "hello")
        self.assertEqual((p.returncode, "not opted in" in p.stderr), (1, True), p.stderr)
        self.assertFalse((self.repo / ".quorum").exists())
        self.assertFalse((Path(self.tmp.name) / "no-vault").exists())

    def test_an_opted_in_repo_works_with_no_vault(self):
        (self.repo / ".quorum").mkdir()
        p = self.cli("repo", "q", "hello")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("a.md", p.stdout)

    def test_sym_from_inside_an_opted_in_repo(self):
        (self.repo / ".quorum").mkdir()
        write(self.repo, "m.py", "def hello_world():\n    pass\n")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-q", "-m", "m")
        p = self.cli("sym", "hello_w")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("m.py:1", p.stdout)


if __name__ == "__main__":
    unittest.main()
