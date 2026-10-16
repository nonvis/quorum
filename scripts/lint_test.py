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

    def test_live_notes_without_summary(self):
        self.assertEqual(self.lint()["no_summary"], ["vault:Note One.md"])

    def test_dead_mentions_into_allowlisted_repos(self):
        d = [(x["note"], x["path"]) for x in self.lint()["dead_mentions"]]
        self.assertEqual(d, [("vault:Note One.md", "~/nonvis/alpha/src/gone.py")])

    def test_memory_unindexed_and_broken_index_rows(self):
        c = self.lint()
        self.assertEqual((c["memory_unindexed"], c["memory_index_broken"]),
                         (["m2.md"], [{"index": "MEMORY.md", "file": "gone.md"}]))

    def test_repo_docs_that_reference_the_vault(self):
        self.assertEqual([(x["repo"], x["doc"]) for x in self.lint()["docs_vault_refs"]],
                         [("alpha", "docs/ARCH.md")])

    def test_repo_docs_naming_files_that_are_gone(self):
        self.assertEqual([(x["doc"], x["file"]) for x in self.lint()["docs_dead_refs"]],
                         [("docs/ARCH.md", "src/missing.py")])

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
