#!/usr/bin/env python3
"""Tests for scripts/lint.py (`quorum lint`) and `quorum q` over repo docs.
Run: python3 scripts/lint_test.py"""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENV = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
           GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def note(path, body, **front):
    fm = "---\n" + "".join("%s: %s\n" % kv for kv in front.items()) + "---\n\n" if front else ""
    write(path, fm + body)


def git(cwd, *args):
    subprocess.run(["git", "-C", str(cwd)] + list(args), check=True, capture_output=True,
                   env=ENV)


class LintCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.home = Path(self.tmp.name)
        self.vault = self.home / "vault"
        self.mem = self.home / "mem"
        repo = self.home / "nonvis" / "alpha"
        write(repo / "src" / "run.py", "def run():\n    pass\n")
        write(repo / "docs" / "ARCH.md", "# Architecture\n\nThe runner is `src/run.py`; the old "
              "`src/missing.py` is gone. Rationale: [[Vault Design Note]], 01_Projects/X.\n")
        write(repo / "docs" / "CLEAN.md", "# Clean\n\nWalrus epochs, see `src/run.py`.\n")
        git(repo, "init", "-q")
        git(repo, "add", ".")
        git(repo, "commit", "-q", "-m", "c")
        write(self.vault / "06_Metadata" / "quorum-repos.txt", "alpha\n")
        note(self.vault / "Note One.md",
             "Links [[Missing Note]] and [[Live]] and [[A - B]]. Runs "
             "`~/nonvis/alpha/src/run.py`; the old ~/nonvis/alpha/src/gone.py is gone.\n",
             title="Note One", locked_in="alpha/docs/ARCH.md", docs="https://docs.example.com/x")
        note(self.vault / "Live.md", "back to [[Note One]], see [[Env]]", title="Live",
             summary='"live"')
        note(self.vault / "A — B.md", "dash note [[Live]]", title="A — B", summary='"dash"')
        note(self.vault / "Locked.md", "points at [[Live]]", title="Locked", summary='"x"',
             locked_in="alpha/docs/NOPE.md")
        note(self.vault / "Archive" / "Old.md", "[[Nothing Here]]", title="Old")
        write(self.mem / "MEMORY.md", "- [one](m1.md)\n- [gone](gone.md)\n")
        note(self.mem / "m1.md", "one", name="m1", description='"one"')
        note(self.mem / "m2.md", "two", name="m2", description='"two"')
        # Memory links use the name: slug, which may differ from the file stem.
        note(self.mem / "feedback_close_ritual.md", "see [[project-quorum-status]]",
             name="feedback-close-ritual", description='"r"')
        note(self.mem / "project_quorum_status.md", "s", name="project-quorum-status",
             description='"s"')
        write(self.mem / "MEMORY.md", "- [one](m1.md)\n- [gone](gone.md)\n"
              "- [r](feedback_close_ritual.md)\n- [s](project_quorum_status.md)\n")
        note(self.mem / "archive" / "old_mem.md", "[[long-gone-slug]]", name="old")
        # A gitignored file on disk (an .env) is alive; ./ prefixes are stripped.
        write(repo / ".env", "X=1\n")
        note(self.vault / "Env.md", "reads ~/nonvis/alpha/.env and [[Live]]", title="Env",
             summary='"env"')
        write(repo / "docs" / "RUN.md", "# Run\n\nStart with `./src/run.py`.\n")
        git(repo, "add", "docs/RUN.md")
        git(repo, "commit", "-q", "-m", "run doc")
        # Dot-dirs (.claude agents/commands) are config, not notes.
        write(self.vault / ".claude" / "agents" / "doer.md", "agent def, no links")

    def tearDown(self):
        self.tmp.cleanup()

    def cli(self, *args):
        env = dict(ENV, HOME=str(self.home), QUORUM_VAULT=str(self.vault),
                   QUORUM_MEMORY=str(self.mem))
        p = subprocess.run([sys.executable, str(HERE / "quorum_cli.py")] + list(args),
                           env=env, capture_output=True, text=True, stdin=subprocess.DEVNULL)
        self.assertEqual(p.returncode, 0, p.stderr)
        return p

    def lint(self, *extra):
        return json.loads(self.cli("lint", "--json", *extra).stdout)["checks"]

    def test_dangling_links_are_listed_with_their_note(self):
        d = {(x["note"], x["target"]) for x in self.lint()["dangling_links"]}
        self.assertEqual(d, {("vault:Note One.md", "Missing Note"),
                             ("vault:Note One.md", "A - B")})

    def test_memory_links_resolve_by_name_slug(self):
        d = {x["target"] for x in self.lint()["dangling_links"]}
        self.assertNotIn("project-quorum-status", d)

    def test_memory_links_are_not_linted_since_unwritten_ones_are_by_convention(self):
        """Memory's rule: a [[name]] with no memory yet marks one worth writing."""
        note(self.mem / "ideas.md", "next: [[some-memory-not-written-yet]]", name="ideas")
        self.assertNotIn("memory:ideas.md", [x["note"] for x in self.lint()["dangling_links"]])

    def test_a_vault_link_with_underscores_for_dashes_resolves(self):
        note(self.vault / "Under.md", "see [[project-quorum-status]] and [[project_quorum_status]]",
             title="Under", summary='"u"')
        self.assertNotIn("vault:Under.md", [x["note"] for x in self.lint()["dangling_links"]])

    def test_archived_sources_are_not_linted_for_dangling_links(self):
        notes = {x["note"] for x in self.lint()["dangling_links"]}
        self.assertFalse({"vault:Archive/Old.md", "memory:archive/old_mem.md"} & notes)

    def test_dot_dirs_are_not_notes(self):
        c = self.lint()
        self.assertFalse([p for p in c["orphans"] + c["no_summary"] if "/.claude/" in p
                          or p.startswith("vault:.claude/")])

    def test_a_gitignored_file_on_disk_is_not_a_dead_mention(self):
        self.assertNotIn("vault:Env.md", [x["note"] for x in self.lint()["dead_mentions"]])

    def test_a_dot_slash_file_in_docs_is_not_dead(self):
        self.assertNotIn("docs/RUN.md", [x["doc"] for x in self.lint()["docs_dead_refs"]])

    def test_orphans_skip_archive_and_count_live_notes_nobody_links(self):
        """'A — B' is only linked by a misspelled [[A - B]] — an orphan until fixed."""
        o = set(self.lint()["orphans"])
        self.assertEqual(o, {"vault:Locked.md", "vault:A — B.md"})
        self.cli("lint", "--fix-safe")
        self.assertEqual(set(self.lint()["orphans"]), {"vault:Locked.md"})

    def test_a_shared_name_links_the_nearest_note_and_a_tie_is_ambiguous(self):
        """[[Plan]] from P/x reaches P/Plan, so Q/Plan is an orphan; from the root
        both are equally near, which no reader can settle."""
        for d in ("P", "Q"):
            note(self.vault / d / "Plan.md", "plan", title="Plan", summary='"p"')
        note(self.vault / "P" / "x.md", "see [[Plan]] [[Note One]]", title="x", summary='"x"')
        note(self.vault / "Root.md", "see [[Plan]] [[Note One]]", title="R", summary='"r"')
        note(self.vault / "Root2.md", "see [[Root]] [[x]]", title="R2", summary='"r"')
        c = self.lint()
        self.assertIn("vault:Q/Plan.md", c["orphans"])
        self.assertEqual(c["ambiguous_links"], [{"note": "vault:Root.md", "target": "Plan"}])
        note(self.vault / "Root2.md", "see [[Root]] [[x]] [[Q/Plan]]", title="R2", summary='"r"')
        self.assertNotIn("vault:Q/Plan.md", self.lint()["orphans"])

    def test_live_notes_without_summary(self):
        self.assertEqual(self.lint()["no_summary"], ["vault:Note One.md"])

    def test_dead_mentions_into_allowlisted_repos(self):
        d = [(x["note"], x["path"]) for x in self.lint()["dead_mentions"]]
        self.assertEqual(d, [("vault:Note One.md", "~/nonvis/alpha/src/gone.py")])

    def test_archived_notes_are_not_linted_for_dead_mentions(self):
        note(self.vault / "Archive" / "Moved.md", "was at `~/nonvis/alpha/src/moved_plan.md`",
             title="Moved")
        d = [x["note"] for x in self.lint()["dead_mentions"]]
        self.assertNotIn("vault:Archive/Moved.md", d)

    def test_templates_and_vault_root_files_are_not_notes_to_lint(self):
        note(self.vault / "06_Metadata" / "Templates" / "T.md", "[[related note]]", title="T")
        write(self.vault / "README.md", "# Vault\n")
        c = self.lint()
        self.assertNotIn("vault:06_Metadata/Templates/T.md", [x["note"] for x in c["dangling_links"]])
        self.assertNotIn("vault:README.md", c["orphans"] + c["no_summary"])

    def test_an_unquoted_colon_in_a_frontmatter_value_breaks_yaml(self):
        write(self.vault / "Bad.md", "---\ntitle: Bad\nsummary: the rule: one writer\n---\nx\n")
        write(self.vault / "Ok.md", "---\ntitle: Ok\nsummary: \"the rule: one writer\"\n"
              "url: https://x.io/a\ntags: [a, b]\nupdated: 2026-10-16  # note: a comment\n"
              "status: open #3 is fine\n---\nx\n")
        c = self.lint()["frontmatter_unquoted_colon"]
        self.assertIn({"note": "vault:Bad.md", "key": "summary"}, c)
        write(self.vault / "Bad2.md", "---\ntitle: B\nsummary: closed; open: #13 creep\n---\nx\n")
        self.assertIn("vault:Bad2.md", [x["note"] for x in self.lint()["frontmatter_unquoted_colon"]])
        self.assertNotIn("vault:Ok.md", [x["note"] for x in c])

    def test_a_quoted_frontmatter_value_must_end_at_its_closing_quote(self):
        write(self.vault / "Q.md", '---\ntitle: Q\nsummary: "MSI zero, tree clean.""\n'
              'status: "open" since 10-30\ndesc: \'it\'s\'\nnote: "never closed\n---\nx\n')
        write(self.vault / "QOk.md", '---\ntitle: QOk\nsummary: "a \\"quoted\\" word: x"\n'
              'desc: \'it\'\'s\'\nupdated: "2026-10-30"  # a comment\nstatus: plain "quote" inside\n'
              '---\nx\n')
        c = self.lint()["frontmatter_bad_quote"]
        self.assertEqual(sorted(x["key"] for x in c if x["note"] == "vault:Q.md"),
                         ["desc", "note", "status", "summary"])
        self.assertNotIn("vault:QOk.md", [x["note"] for x in c])
        write(self.vault / "QLong.md", '---\ntitle: L\nsummary: "%s"\n---\nx\n' % ("word " * 2000))
        self.assertNotIn("vault:QLong.md", [x["note"] for x in self.lint()["frontmatter_bad_quote"]])

    def test_memory_unindexed_and_broken_index_rows(self):
        c = self.lint()
        self.assertEqual((c["memory_unindexed"], c["memory_index_broken"]),
                         (["m2.md"], [{"index": "MEMORY.md", "file": "gone.md"}]))

    def test_a_memory_file_an_index_wikilinks_by_name_is_indexed(self):
        write(self.mem / "MEMORY_X.md", "- row with ☠ [[m2]] inline\n")
        self.assertEqual(self.lint()["memory_unindexed"], [])

    def test_repo_docs_that_reference_the_vault(self):
        self.assertEqual([(x["repo"], x["doc"]) for x in self.lint()["docs_vault_refs"]],
                         [("alpha", "docs/ARCH.md")])

    def test_a_frozen_record_listed_by_exact_path_is_not_a_vault_ref(self):
        repo = self.home / "nonvis" / "alpha"
        for d in ("run-1", "run-2"):
            write(repo / "measurements" / d / "PRE-REGISTRATION.md", "# P\n\nper Ops/135.\n")
        git(repo, "add", "measurements")
        git(repo, "commit", "-q", "-m", "records")
        write(self.vault / "06_Metadata" / "quorum-lint-frozen.txt",
              "# frozen records\nalpha/measurements/run-1/PRE-REGISTRATION.md  # run 10-21\n"
              "alpha/docs/ARCH.md\n")
        refs = [x["doc"] for x in self.lint()["docs_vault_refs"]]
        # listed paths are skipped; the folder is not, so a new record is caught
        self.assertNotIn("measurements/run-1/PRE-REGISTRATION.md", refs)
        self.assertNotIn("docs/ARCH.md", refs)
        self.assertIn("measurements/run-2/PRE-REGISTRATION.md", refs)

    def test_repo_docs_naming_files_that_are_gone(self):
        self.assertEqual([(x["doc"], x["file"]) for x in self.lint()["docs_dead_refs"]],
                         [("docs/ARCH.md", "src/missing.py")])

    def doc_case(self, name, body):
        repo = self.home / "nonvis" / "alpha"
        write(repo / "docs" / name, body)
        git(repo, "add", "docs/" + name)
        git(repo, "commit", "-q", "-m", "doc " + name)
        r = self.lint()
        return ([x["doc"] for x in r["docs_vault_refs"]],
                [(x["doc"], x["file"]) for x in r["docs_dead_refs"]])

    def test_shorthand_vault_cites_are_vault_refs(self):
        for i, line in enumerate(["retired at MVP (Sui)/37.", "see Ops/135 G10.",
                                  "per vault `Meridian/Raft/03` §4.", "Citable numbers: vault Pitch/06.",
                                  "from Action Queue/10.", "the ledger at ~/Claude/Projects/X/ledger.md"]):
            refs, _ = self.doc_case("S%d.md" % i, "# S\n\n%s\n" % line)
            self.assertIn("docs/S%d.md" % i, refs, line)

    def test_vault_note_ids_are_vault_refs(self):
        for i, line in enumerate(["see Sidebar/35 (SD-66).", "per QA/43.", "(vault Permafrost/37, r30)",
                                  "closed by AQ/79.", "see DTK/31 §4.", "per PB/02.", "Broker/06 rules it.",
                                  "Leadsman/38 found it.", "vault Operations/97 has it.",
                                  "A Day With/06 walks it."]):
            refs, _ = self.doc_case("N%d.md" % i, "# N\n\n%s\n" % line)
            self.assertIn("docs/N%d.md" % i, refs, line)

    def test_bare_names_and_paths_outside_tracked_top_dirs_are_not_checked(self):
        _, dead = self.doc_case("B.md", "# B\n\nReads `keys.h`, `client.yaml` and "
                                "`rgw/rgw_op.cc`; runs `src/gone_now.py`.\n")
        self.assertEqual([d[1] for d in dead if d[0] == "docs/B.md"], ["src/gone_now.py"])

    def test_a_ref_on_a_line_naming_a_repo_outside_the_index_is_not_checked(self):
        """`beta` is a repo the vault names but does not index: its files can't be
        looked up, so a ref beside it is unknown, not dead."""
        note(self.vault / "Beta.md", "beta: ~/nonvis/beta/README.md, [[Live]]", title="Beta",
             summary='"b"')
        _, dead = self.doc_case("X.md", "# X\n\n| feed | `beta` `src/resolve_feed.ts` |\n\n"
                                "Runs `src/gone_too.py`.\n")
        self.assertEqual([d[1] for d in dead if d[0] == "docs/X.md"], ["src/gone_too.py"])

    def test_a_broken_relative_markdown_link_is_a_dead_ref(self):
        _, dead = self.doc_case("L.md", "# L\n\nSee [ops](server-operations.md), "
                                "[arch](ARCH.md) and [site](https://x.io/a.md).\n")
        self.assertEqual([d[1] for d in dead if d[0] == "docs/L.md"], ["docs/server-operations.md"])

    def test_placeholders_and_example_twins_are_not_dead(self):
        repo = self.home / "nonvis" / "alpha"
        write(repo / "src" / "keys.h.example", "x")
        git(repo, "add", "src/keys.h.example")
        _, dead = self.doc_case("P.md", "# P\n\nCopy `src/keys.h`; logs go to "
                                "`src/YYYY-MM-DD-run.md` and `src/<bot>_status.sh`.\n")
        self.assertEqual([d for d in dead if d[0] == "docs/P.md"], [])

    def test_cpp_attributes_and_code_are_not_wikilinks(self):
        refs, _ = self.doc_case("CPP.md", "# C\n\nUse `[[nodiscard]]` and\n```cpp\n"
                                "[[maybe_unused]] int x;\n```\nThe Sui vault contract holds funds.\n")
        self.assertNotIn("docs/CPP.md", refs)

    def test_wikilinks_to_markdown_files_in_the_repo_are_not_vault_refs(self):
        repo = self.home / "nonvis" / "alpha"
        write(repo / "docs" / "Sibling note.md", "# Sib\n")
        git(repo, "add", "docs/Sibling note.md")
        refs, _ = self.doc_case("W.md", "# W\n\nSee [[Sibling note]] and [[Sibling note|it]].\n")
        self.assertNotIn("docs/W.md", refs)
        refs, _ = self.doc_case("W2.md", "# W\n\nSee [[Sibling note]] and [[Some Vault Note]].\n")
        self.assertIn("docs/W2.md", refs)

    def test_a_backticked_config_default_is_not_a_vault_ref(self):
        refs, _ = self.doc_case("CFG.md", "# C\n\nThe vault path defaults to `~/nonvis/second-brain`.\n")
        self.assertNotIn("docs/CFG.md", refs)

    def test_retired_files_runtime_outputs_and_relative_paths_are_not_dead(self):
        _, dead = self.doc_case("D.md", "# D\n\n`old.sh` was retired. Writes `report.json`. "
                                "See `../src/run.py`.\n")
        self.assertEqual([d for d in dead if d[0] == "docs/D.md"], [])

    def test_every_tracked_markdown_file_is_checked_not_only_docs(self):
        repo = self.home / "nonvis" / "alpha"
        write(repo / "demo-app" / "services" / "README.md", "# S\n\nSee vault Permafrost/37 D-6.\n")
        write(repo / "TASKS.md", "# T\n\nRun `src/gone_too.py`.\n")
        git(repo, "add", ".")
        git(repo, "commit", "-q", "-m", "more md")
        c = self.lint()
        self.assertIn("demo-app/services/README.md", [x["doc"] for x in c["docs_vault_refs"]])
        self.assertIn(("TASKS.md", "src/gone_too.py"),
                      [(x["doc"], x["file"]) for x in c["docs_dead_refs"]])

    def test_a_file_the_line_calls_generated_or_gitignored_is_not_dead(self):
        _, dead = self.doc_case("G.md", "# G\n\nThe launcher writes `src/out.live.toml` "
                                "(generated, gitignored).\n")
        self.assertEqual([d for d in dead if d[0] == "docs/G.md"], [])

    def test_a_locked_doc_that_is_missing(self):
        self.assertEqual([(x["note"], x["doc"]) for x in self.lint()["locked_docs_missing"]],
                         [("vault:Locked.md", "alpha/docs/NOPE.md")])

    def test_fix_safe_repairs_only_an_unambiguous_spelling_variant(self):
        self.cli("lint", "--fix-safe")
        text = (self.vault / "Note One.md").read_text()
        self.assertIn("[[A — B]]", text)
        self.assertIn("[[Missing Note]]", text)
        d = {x["target"] for x in self.lint()["dangling_links"]}
        self.assertNotIn("A - B", d)

    def test_fix_safe_quotes_an_unquoted_colon_value(self):
        write(self.vault / "Bad.md", '---\ntitle: Bad\nsummary: the rule: "one" writer\n---\nbody: x\n')
        self.cli("lint", "--fix-safe")
        self.assertEqual((self.vault / "Bad.md").read_text(),
                         '---\ntitle: Bad\nsummary: "the rule: \\"one\\" writer"\n---\nbody: x\n')
        self.assertEqual(self.lint()["frontmatter_unquoted_colon"], [])

    def test_lint_human_output_names_every_check(self):
        out = self.cli("lint").stdout
        for k in ("dangling links", "orphans", "no summary", "dead mentions", "memory",
                  "docs → vault", "docs dead refs", "locked docs missing"):
            self.assertIn(k, out)

    def test_q_also_searches_allowlisted_repo_docs(self):
        hits = json.loads(self.cli("q", "walrus", "epochs", "--json").stdout)
        self.assertIn("repo:alpha:docs/CLEAN.md", [h["path"] for h in hits])

    def test_q_vault_only_leaves_repo_docs_out(self):
        hits = json.loads(self.cli("q", "walrus", "epochs", "--json", "--vault-only").stdout)
        self.assertFalse([h for h in hits if h["path"].startswith("repo:")])


if __name__ == "__main__":
    unittest.main()
