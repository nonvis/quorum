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

    def test_where_from_inside_an_opted_in_repo(self):
        (self.repo / ".quorum").mkdir()
        write(self.repo, "m.py", "def hello_world():\n    pass\n\nhello_world()\n")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-q", "-m", "m")
        p = self.cli("where", "hello_world")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("m.py:1", p.stdout)
        self.assertIn("m.py:4", p.stdout)
        self.assertIn("1 def · 1 ref", p.stdout)


class WhereCase(unittest.TestCase):
    """`where`: every place an identifier occurs in the committed code, by role."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name) / "gamma"
        self.repo.mkdir()
        git(self.repo, "init", "-q", "-b", "main")
        write(self.repo, "src/sweep.rs",
              "use crate::cfg::Config;\n"
              "/* sweep_once is the loop body\n   (see sweep_once below) */\n"
              "pub fn sweep_once<'a>(cfg: &'a Config) -> u64 {\n"
              "    let label = \"sweep_once\";\n"
              "    let c: char = 'x';\n"
              "    label.len() as u64\n}\n")
        write(self.repo, "src/main.rs",
              "use crate::sweep::sweep_once;\n"
              "fn main() {\n"
              "    // call sweep_once forever\n"
              "    loop { sweep_once(&cfg); }\n"
              "    let raw = r#\"sweep_once \"quoted\"\"#;\n"
              "}\n")
        write(self.repo, "lib/book.py",
              "from lib.sweep import sweep_once\n\n"
              "class Book:\n"
              "    \"\"\"Book wraps sweep_once.\"\"\"\n"
              "    def total(self):\n"
              "        return sweep_once(self)  # sweep_once again\n"
              "\n"
              "def Total():\n"
              "    return 1\n")
        write(self.repo, "web/app.ts",
              "import { sweepOnce } from './api';\n"
              "export const run = () => sweepOnce(`${sweepOnce.name}`);\n")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-q", "-m", "first")
        self.idx = ri.RepoIndex(self.repo)
        self.idx.refresh()

    def tearDown(self):
        self.idx.close()
        self.tmp.cleanup()

    def where(self, name):
        return [(h["role"], "%s:%d" % (h["path"], h["line"])) for h in self.idx.where(name)["hits"]]

    def test_every_occurrence_by_role_definitions_first(self):
        self.assertEqual(self.where("sweep_once"), [
            ("def", "src/sweep.rs:4"),
            ("import", "lib/book.py:1"),
            ("import", "src/main.rs:1"),
            ("ref", "lib/book.py:6"),
            ("ref", "src/main.rs:4"),
            ("text", "lib/book.py:4"),
            ("text", "lib/book.py:6"),
            ("text", "src/main.rs:3"),
            ("text", "src/main.rs:5"),
            ("text", "src/sweep.rs:2"),
            ("text", "src/sweep.rs:3"),
            ("text", "src/sweep.rs:5"),
        ])

    def test_a_hit_carries_its_line_text_and_column(self):
        h = [h for h in self.idx.where("sweep_once")["hits"] if h["role"] == "ref"][1]
        self.assertEqual((h["text"], h["col"]), ("loop { sweep_once(&cfg); }", 12))

    def test_rust_lifetimes_and_char_literals_do_not_open_a_string(self):
        self.assertIn(("ref", "src/sweep.rs:4"), self.where("cfg"))
        self.assertIn(("ref", "src/sweep.rs:4"), self.where("Config"))
        self.assertIn(("ref", "src/sweep.rs:7"), self.where("label"))

    def test_a_backslash_newline_inside_a_string_does_not_end_it(self):
        text = 'assert!(ok, "first \\\n   second");\nlet after_it = f("x");\n'
        self.assertIn(("after_it", 3, 5, "ref"), ri.occurrences_of("t.rs", text, set()))

    def roles(self, path, text, name):
        return [(ln, role) for n, ln, _, role in ri.occurrences_of(path, text, set()) if n == name]

    def test_nested_templates_and_braces_inside_an_interpolation(self):
        text = ("const a = `x ${ok ? `y ${inner}` : fn({k: 1})} z`;\n"
                "const after_it = `w`;\n")
        self.assertEqual(self.roles("t.ts", text, "inner"), [(1, "ref")])
        self.assertEqual(self.roles("t.ts", text, "after_it"), [(2, "ref")])

    def test_a_regex_literal_holding_a_quote_does_not_open_a_string(self):
        text = "const re = /[`'\"]/g;\nconst after_it = re.test(s);\n"
        self.assertEqual(self.roles("t.ts", text, "after_it"), [(2, "ref")])

    def test_cpp_raw_string_and_rust_raw_string(self):
        cpp = 'auto q = R"SQL(\n  SELECT "x" FROM t_name;\n)SQL";\nint after_it = 1;\n'
        self.assertEqual(self.roles("t.cc", cpp, "t_name"), [(2, "text")])
        self.assertEqual(self.roles("t.cc", cpp, "after_it"), [(4, "ref")])
        rs = 'let q = r#"say "t_name""#;\nlet after_it = 1;\n'
        self.assertEqual(self.roles("t.rs", rs, "t_name"), [(1, "text")])
        self.assertEqual(self.roles("t.rs", rs, "after_it"), [(2, "ref")])

    def test_a_shell_heredoc_is_text(self):
        sh = "cat <<'EOF' > out_file\nit's run_once here\nEOF\nrun_once \"$after_it\"\n"
        self.assertEqual(self.roles("t.sh", sh, "run_once"), [(2, "text"), (4, "ref")])
        self.assertEqual(self.roles("t.sh", sh, "out_file"), [(1, "ref")])
        self.assertEqual(self.roles("t.sh", sh, "after_it"), [(4, "ref")])

    def test_template_interpolation_is_code(self):
        self.assertEqual(self.where("sweepOnce"),
                         [("import", "web/app.ts:1"), ("ref", "web/app.ts:2")])

    def test_exact_case_first_else_case_insensitive(self):
        r = self.idx.where("total")
        self.assertEqual((r["exact"], [h["line"] for h in r["hits"]]), (True, [5]))
        r = self.idx.where("TOTAL")
        self.assertEqual((r["exact"], sorted(h["line"] for h in r["hits"])), (False, [5, 8]))

    def test_a_qualified_name_looks_up_its_last_part(self):
        self.assertEqual(self.where("crate::sweep::sweep_once"), self.where("sweep_once"))
        self.assertEqual(self.where("Book.total"), self.where("total"))

    def test_prose_words_in_comments_and_strings_are_not_stored(self):
        self.assertEqual((self.where("forever"), self.where("quoted"), self.where("wraps")),
                         ([], [], []))

    def test_keywords_and_one_letter_names_are_not_stored(self):
        self.assertEqual((self.where("fn"), self.where("c")), ([], []))

    def test_a_changed_file_is_reindexed(self):
        write(self.repo, "src/main.rs", "fn main() {}\n")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-q", "-m", "drop the call")
        self.idx.refresh()
        self.assertNotIn("src/main.rs", {p.split(":")[0] for _, p in self.where("sweep_once")})

    def test_an_uncommitted_edit_marks_its_hits_dirty(self):
        write(self.repo, "src/main.rs", "fn main() {}\n")
        dirty = {h["path"]: h["dirty"] for h in self.idx.where("sweep_once")["hits"]}
        self.assertEqual((dirty["src/main.rs"], dirty["src/sweep.rs"]), (True, False))


class AffectedCase(unittest.TestCase):
    """`affected`: the tests a change reaches through imports (files) or packages."""

    FILES = {
        # python: absolute from the importer's ancestors, relative, package __init__
        "lib/__init__.py": "",
        "lib/core.py": "def base():\n    return 1\n",
        "lib/util.py": "from lib import core\n\ndef helper():\n    return core.base()\n",
        "lib/rel.py": "from .core import base\n",
        "tests/test_util.py": "from lib.util import helper\n",
        "tests/test_rel.py": "import lib.rel as r\n",
        "tests/test_other.py": "import json\n",
        # ts: relative (.js → .ts), workspace package by name + exports subpath
        "packages/core/package.json": '{"name": "@x/core", "exports": {".": "./src/index.ts",'
                                      ' "./extra": {"import": "./src/extra.ts"}}}\n',
        "packages/core/src/index.ts": "export * from './money.js';\n",
        "packages/core/src/money.ts": "export const money = 1;\n",
        "packages/core/src/extra.ts": "export const extra = 2;\n",
        "apps/web/src/view.tsx": "import { money } from '@x/core';\nexport const View = money;\n",
        "apps/web/src/view.test.tsx": "import { View } from './view';\n",
        "apps/web/e2e/extra.spec.ts": "const { extra } = require('@x/core/extra');\n",
        # c++: include by the importer's dir, else by a unique path suffix
        "src/book.h": "struct Book {};\n",
        "src/book.cc": '#include "book.h"\n#include <vector>\n',
        "tests/book_test.cc": '#include "book.h"\n',
        # shell: source / .
        "scripts/lib.sh": "say() { echo hi; }\n",
        "scripts/gate.sh": 'source "$(dirname "$0")/lib.sh"\n',
        "tests/gate_test.sh": ". ../scripts/gate.sh\n",
        # rust: crate-level, path deps
        "crates/a/Cargo.toml": '[package]\nname = "a"\n',
        "crates/a/src/lib.rs": "mod inner;\n",
        "crates/a/src/inner.rs": "pub fn f() {}\n",
        "crates/a/tests/it.rs": "#[test]\nfn t() { a::f(); }\n",
        "crates/b/Cargo.toml": '[package]\nname = "b"\n[dependencies]\na = { path = "../a" }\n',
        "crates/b/src/lib.rs": "pub fn g() {}\n",
        "crates/b/tests/b_it.rs": "#[test]\nfn t() {}\n",
        # move: package-level, local deps
        "move/p/Move.toml": '[package]\nname = "p"\n',
        "move/p/sources/p.move": "module p::p { public fun f() {} }\n",
        "move/p/tests/p_tests.move": "#[test_only]\nmodule p::p_tests {}\n",
        "move/q/Move.toml": '[package]\nname = "q"\n[dependencies]\np = { local = "../p" }\n',
        "move/q/sources/q.move": "module q::q {}\n",
        "README.md": "# delta\n",
    }

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name) / "delta"
        self.repo.mkdir()
        git(self.repo, "init", "-q", "-b", "main")
        for rel, text in self.FILES.items():
            write(self.repo, rel, text)
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-q", "-m", "first")
        self.idx = ri.RepoIndex(self.repo)
        self.idx.refresh()

    def tearDown(self):
        self.idx.close()
        self.tmp.cleanup()

    def tests(self, *changed):
        return sorted(t["path"] for t in self.idx.affected(list(changed))["tests"])

    def test_python_through_an_absolute_import_chain(self):
        self.assertEqual(self.tests("lib/core.py"), ["tests/test_rel.py", "tests/test_util.py"])

    def test_a_hit_names_its_changed_origin_and_hop_count(self):
        t = {t["path"]: t for t in self.idx.affected(["lib/core.py"])["tests"]}
        self.assertEqual((t["tests/test_util.py"]["origin"], t["tests/test_util.py"]["hops"]),
                         ("lib/core.py", 2))

    def test_a_changed_test_is_its_own_hit(self):
        self.assertEqual(self.tests("tests/test_other.py"), ["tests/test_other.py"])

    def test_ts_through_a_workspace_package_and_its_reexport(self):
        self.assertEqual(self.tests("packages/core/src/money.ts"), ["apps/web/src/view.test.tsx"])

    def test_a_package_name_two_dirs_share_resolves_to_the_nearer(self):
        tree = ri.repo_deps.Tree(
            ["packages/core/src/index.ts", "parked/x/packages/core/src/index.ts"],
            {"packages/core/package.json": '{"name": "@x/core"}',
             "parked/x/packages/core/package.json": '{"name": "@x/core"}'})
        self.assertEqual(ri.repo_deps.resolve(tree, "apps/web/a.ts", "@x/core", "js"),
                         ["packages/core/src/index.ts"])
        self.assertEqual(ri.repo_deps.resolve(tree, "parked/x/apps/web/a.ts", "@x/core", "js"),
                         ["parked/x/packages/core/src/index.ts"])

    def test_ts_through_an_exports_subpath(self):
        self.assertEqual(self.tests("packages/core/src/extra.ts"), ["apps/web/e2e/extra.spec.ts"])

    def test_cpp_include_and_shell_source(self):
        self.assertEqual(self.tests("src/book.h"), ["tests/book_test.cc"])
        self.assertEqual(self.tests("scripts/lib.sh"), ["tests/gate_test.sh"])

    def test_rust_is_by_crate_and_follows_path_dependents(self):
        r = self.idx.affected(["crates/a/src/inner.rs"])
        self.assertEqual(sorted(p["path"] for p in r["packages"]), ["crates/a", "crates/b"])
        self.assertEqual(sorted(t["path"] for t in r["tests"]),
                         ["crates/a/tests/it.rs", "crates/b/tests/b_it.rs"])
        r = self.idx.affected(["crates/b/src/lib.rs"])
        self.assertEqual([p["path"] for p in r["packages"]], ["crates/b"])

    def test_move_is_by_package_and_follows_local_dependents(self):
        r = self.idx.affected(["move/p/sources/p.move"])
        self.assertEqual(sorted(p["path"] for p in r["packages"]), ["move/p", "move/q"])
        self.assertEqual([t["path"] for t in r["tests"]], ["move/p/tests/p_tests.move"])

    def test_a_change_no_test_reaches_is_reported(self):
        r = self.idx.affected(["README.md", "lib/core.py"])
        self.assertEqual(r["unreached"], ["README.md"])

    def test_a_changed_file_named_from_a_subdir_or_absolute_path(self):
        self.assertEqual(self.tests(str(self.repo / "lib" / "core.py")),
                         ["tests/test_rel.py", "tests/test_util.py"])

    def test_the_default_change_set_is_the_uncommitted_edits(self):
        write(self.repo, "lib/core.py", "def base():\n    return 2\n")
        write(self.repo, "lib/new.py", "x = 1\n")
        self.assertEqual(sorted(self.idx.changed_files()), ["lib/core.py", "lib/new.py"])
        (self.repo / ".quorum").mkdir(exist_ok=True)
        p = subprocess.run([sys.executable, str(HERE / "quorum_cli.py"), "affected", "--quiet"],
                           cwd=str(self.repo), capture_output=True, text=True,
                           env=dict(ENV, QUORUM_VAULT=str(Path(self.tmp.name) / "no-vault")))
        self.assertEqual((p.returncode, p.stdout.split()),
                         (0, ["tests/test_rel.py", "tests/test_util.py"]), p.stderr)

    def test_test_path_conventions(self):
        for p in ("tests/x.rs", "a/test_x.py", "a/x_test.py", "a/x.test.ts", "a/x.spec.tsx",
                  "a/x_test.go", "a/__tests__/x.js", "e2e/x.mts", "a/x_unittest.cc"):
            self.assertTrue(ri.is_test_path(p), p)
        for p in ("a/latest.py", "a/contest.ts", "src/testing_utils.rs"):
            self.assertFalse(ri.is_test_path(p), p)


if __name__ == "__main__":
    unittest.main()
