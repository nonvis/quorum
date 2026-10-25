#!/usr/bin/env python3
"""Tests for scripts/vault_index.py. Run: python3 scripts/vault_index_test.py"""
import json
import re
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



class LinkTargetsCase(unittest.TestCase):
    def test_links_inside_code_are_not_links(self):
        body = ("See [[Real]].\n```sh\nif [[ ! -f \"$X\" ]]; then\n```\n"
                "Inline `[[ -n $Y ]]` too, and [[Also Real|alias]].\n")
        self.assertEqual(vi.link_targets(body), ["Real", "Also Real"])

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

    def test_an_archived_memory_file_ranks_after_a_live_one(self):
        """memory/archive/ is the memory dir's Archive/."""
        note(self.mem / "archive" / "old_lease.md", "walrus lease renewal; walrus lease renewal",
             name="old_lease")
        note(self.mem / "live_lease.md", "walrus lease renewal", name="live_lease")
        self.idx.refresh()
        self.assertEqual(self.paths(self.idx.search("walrus lease renewal", source="memory")),
                         ["memory:live_lease.md", "memory:archive/old_lease.md"])

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

    def test_a_shared_name_resolves_to_the_note_beside_the_linker(self):
        note(self.vault / "01_Projects" / "Crucible" / "06 - Decision Log.md", "Crucible's log.")
        self.idx.refresh()
        out = self.idx.links("vault:01_Projects/Quorum/15 - Briefing.md")["outgoing"]
        self.assertIn(("06 - Decision Log", "vault:01_Projects/Quorum/06 - Decision Log.md"),
                      [(o["target"], o["path"]) for o in out])
        back = self.idx.links("vault:01_Projects/Crucible/06 - Decision Log.md")["backlinks"]
        self.assertEqual(back, [])

    def test_a_shared_name_prefers_the_nearest_folder_then_a_path_in_the_link(self):
        note(self.vault / "01_Projects" / "Crucible" / "02 - Decisions.md", "c")
        note(self.vault / "01_Projects" / "Permafrost" / "02 - Decisions.md", "p")
        note(self.vault / "01_Projects" / "Permafrost" / "MVP" / "40 - Run.md",
             "See [[02 - Decisions]] and [[Crucible/02 - Decisions|theirs]].")
        self.idx.refresh()
        out = self.idx.links("40 - Run")["outgoing"]
        self.assertEqual([o["path"] for o in out],
                         ["vault:01_Projects/Permafrost/02 - Decisions.md",
                          "vault:01_Projects/Crucible/02 - Decisions.md"])

    def test_a_note_in_the_linkers_folder_beats_one_in_a_subfolder(self):
        for d in ("Bots", "Bots/MM"):
            note(self.vault / d / "00 - Overview.md", d)
        note(self.vault / "Bots" / "02 - Patterns.md", "[[00 - Overview]]")
        self.idx.refresh()
        self.assertEqual([o["path"] for o in self.idx.links("02 - Patterns")["outgoing"]],
                         ["vault:Bots/00 - Overview.md"])
        self.assertFalse(any(a for _, _, _, a in self.idx.resolved_links()))

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
        # The shortest prefix (>= 8) holding a digit and a letter, as a real cite
        # would be; an all-digit 8-char prefix is (rightly) not taken for a sha.
        self.short = next(self.sha[:n] for n in range(8, 41)
                          if re.search(r"[0-9]", self.sha[:n]) and re.search(r"[a-f]", self.sha[:n]))
        git(self.repo, "worktree", "add", "-q", str(self.home / "nonvis" / "alpha-wt"))
        (self.home / "nonvis" / "datadir").mkdir()
        inner = self.home / "projects" / "fluid" / "fluid-pkgs"
        inner.mkdir(parents=True)
        git(inner, "init", "-q")
        self.vault = self.home / "vault"
        note(self.vault / "A.md",
             "Ran `~/nonvis/alpha/scripts/run.py` at `%s`; see PR #12." % self.short,
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

    def test_a_root_cloned_after_it_was_recorded_absent_becomes_a_repo(self):
        """A clone changes no note; the next refresh must still see it."""
        gone = self.home / "nonvis" / "gone"
        gone.mkdir()
        git(gone, "init", "-q")
        self.idx.refresh()
        self.assertEqual(self.repos()["gone"]["status"], "repo")

    def test_a_non_git_root_that_becomes_a_repo_is_re_resolved(self):
        git(self.home / "nonvis" / "datadir", "init", "-q")
        self.idx.refresh()
        self.assertEqual(self.repos()["datadir"]["status"], "repo")

    def test_a_removed_repo_becomes_absent(self):
        import shutil
        shutil.rmtree(self.home / "projects" / "fluid")
        self.idx.refresh()
        st = {r["name"]: r["status"] for r in self.idx.repos()}
        self.assertNotEqual(st.get("fluid-pkgs"), "repo")

    def test_a_dot_named_path_is_not_a_repo(self):
        """~/nonvis/.host-busy-alpha is a lock file, not a missing repo."""
        note(self.vault / "D.md", "The lock `~/nonvis/.host-busy-alpha` and ~/nonvis/.host-busy-.",
             title="D")
        self.idx.refresh()
        self.assertEqual([n for n in self.repos() if n.startswith(".")], [])

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


class HookCase(unittest.TestCase):
    """Step 4: `quorum hook` — the UserPromptSubmit hook. It injects a project's
    entry points (its memory index, its briefing) the first time the operator
    names the project in a session, and a note named by `Folder/NN`; topic words
    alone inject nothing (the 10-17 replay: 2 % of word-matched hits were read)."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.vault = root / "vault"
        self.mem = root / "mem"
        self.mem.mkdir()
        (self.mem / "MEMORY_PERMAFROST.md").write_text(
            "---\nname: permafrost-index\ndescription: Permafrost's memory index\n---\n\nhooks\n")
        note(self.vault / "01_Projects" / "Permafrost" / "28 - Briefing.md", "where it is",
             title="Briefing", summary='"Where Permafrost is"')
        note(self.vault / "01_Projects" / "Permafrost" / "07 - Litestream Restore.md",
             "The restore drill: stop writers before litestream restores the WAL segments.",
             title="Litestream Restore", summary='"How the restore drill runs"')
        note(self.vault / "01_Projects" / "Partners" / "Samsung" / "21 - Bastion Briefing.md",
             "listener", title="Bastion Briefing", summary='"Where Bastion is"')
        note(self.vault / "01_Projects" / "Old" / "00 - Old.md", "old", title="Old")
        note(self.vault / "Action Queue" / "94 - Loqua asks.md", "asks", title="Loqua asks",
             summary='"Every open ask"')
        note(self.vault / "Archive" / "Permafrost" / "07 - Old Restore.md", "old", title="Old")
        (self.vault / "06_Metadata").mkdir()
        (self.vault / "06_Metadata" / "quorum-projects.json").write_text(json.dumps({
            "v": 1, "projects": {
                "permafrost": {"name": "Permafrost", "folders": ["01_Projects/Permafrost"],
                               "notes": ["01_Projects/Permafrost/28 - *.md"]},
                "bastion": {"name": "Bastion", "folders": ["01_Projects/Partners/Samsung"],
                            "notes": ["01_Projects/Partners/Samsung/21 - *.md"]},
                "old": {"name": "Old", "folders": ["01_Projects/Old"],
                        "notes": ["01_Projects/Old/00 - *.md"], "active": False}}}))

    def tearDown(self):
        self.tmp.cleanup()

    def hook(self, prompt, extra_env=None, raw=None, session="sess-1"):
        env = dict(os.environ, QUORUM_VAULT=str(self.vault), QUORUM_MEMORY=str(self.mem))
        env.update(extra_env or {})
        payload = raw if raw is not None else json.dumps(
            {"prompt": prompt, "session_id": session, "hook_event_name": "UserPromptSubmit"})
        return subprocess.run([sys.executable, str(HERE / "quorum_cli.py"), "hook"],
                              input=payload, env=env, capture_output=True, text=True)

    def context(self, p):
        self.assertEqual(p.returncode, 0, p.stderr)
        if not p.stdout.strip():
            return ""
        out = json.loads(p.stdout)["hookSpecificOutput"]
        self.assertEqual(out["hookEventName"], "UserPromptSubmit")
        return out["additionalContext"]

    def test_naming_a_project_injects_its_memory_index_then_briefing(self):
        c = self.context(self.hook("let's work on permafrost"))
        self.assertIn("memory:MEMORY_PERMAFROST.md — Permafrost's memory index", c)
        self.assertIn("28 - Briefing.md — Where Permafrost is", c)
        self.assertLess(c.index("MEMORY_PERMAFROST"), c.index("28 - Briefing"))
        self.assertNotIn("Litestream", c)

    def test_a_projects_folder_name_names_it_too(self):
        self.assertIn("21 - Bastion Briefing.md",
                      self.context(self.hook("for samsung project, how is the listener?")))

    def test_a_word_that_only_contains_the_name_is_no_mention(self):
        self.assertEqual(self.context(self.hook("permafrosty mornings")), "")

    def test_a_project_named_past_the_first_ten_words_is_a_passing_mention(self):
        self.assertEqual(self.context(self.hook(
            "let's remove the unused volumes, it seems safe, then deploy to the permafrost box")), "")
        self.assertIn("07 - Litestream Restore.md", self.context(self.hook(
            "let's remove the unused volumes, it seems safe, then see permafrost/07")))

    def test_topic_words_without_a_project_inject_nothing(self):
        self.assertEqual(self.context(self.hook(
            "how does the litestream restore drill handle the WAL segments?")), "")

    def test_an_inactive_project_injects_nothing(self):
        self.assertEqual(self.context(self.hook("anything left on old?")), "")

    def test_a_project_named_only_in_pasted_text_injects_nothing(self):
        self.assertEqual(self.context(self.hook(
            "<pasted_content id=\"a\">permafrost deploy log</pasted_content> what now?")), "")

    def test_a_folder_and_number_injects_that_note(self):
        c = self.context(self.hook("see permafrost/07 and AQ/94"))
        self.assertIn("07 - Litestream Restore.md", c)
        self.assertIn("94 - Loqua asks.md", c)
        self.assertNotIn("Archive/", c)

    def test_a_note_is_injected_once_per_session(self):
        self.assertIn("28 - Briefing", self.context(self.hook("let's work on permafrost")))
        self.assertEqual(self.context(self.hook("back to permafrost")), "")
        self.assertIn("28 - Briefing",
                      self.context(self.hook("permafrost again", session="sess-2")))

    def test_the_context_is_capped(self):
        refs = " ".join("permafrost/%02d" % i for i in range(30, 60))
        for i in range(30, 60):
            note(self.vault / "01_Projects" / "Permafrost" / ("%02d - N.md" % i), "n",
                 title="N", summary='"' + "x" * 400 + '"')
        c = self.context(self.hook(refs))
        self.assertLessEqual(c.count("\n- "), 5)
        self.assertLessEqual(len(c), 4000)

    def test_a_harness_message_is_not_a_prompt(self):
        for tag in ("task-notification", "system-reminder"):
            p = "<%s> permafrost done </%s>" % (tag, tag)
            self.assertEqual(self.context(self.hook(p)), "", tag)

    def test_no_projects_file_injects_nothing(self):
        (self.vault / "06_Metadata" / "quorum-projects.json").unlink()
        self.assertEqual(self.context(self.hook("let's work on permafrost")), "")

    def test_off_switch(self):
        p = self.hook("let's work on permafrost", {"QUORUM_HOOK": "off"})
        self.assertEqual((p.returncode, p.stdout), (0, ""))

    def test_bad_input_or_no_vault_exits_0_silently(self):
        p = self.hook(None, raw="not json")
        self.assertEqual((p.returncode, p.stdout), (0, ""))
        p = self.hook("permafrost", {"QUORUM_VAULT": str(self.vault / "nope")})
        self.assertEqual((p.returncode, p.stdout), (0, ""))
        self.assertFalse((self.vault / "nope").exists())

    def toggle(self, word):
        env = dict(os.environ, QUORUM_VAULT=str(self.vault), QUORUM_MEMORY=str(self.mem))
        return subprocess.run([sys.executable, str(HERE / "quorum_cli.py"), "hook", word],
                              env=env, capture_output=True, text=True, stdin=subprocess.DEVNULL)

    def test_hook_off_persists_until_on_and_status_says_which(self):
        self.toggle("off")
        self.assertIn("off", self.toggle("status").stdout)
        self.assertEqual(self.context(self.hook("let's work on permafrost")), "")
        self.toggle("on")
        self.assertIn("on", self.toggle("status").stdout)
        self.assertIn("28 - Briefing", self.context(self.hook("let's work on permafrost")))

    def test_an_off_call_is_logged_as_off_without_hits(self):
        self.toggle("off")
        self.hook("let's work on permafrost")
        self.hook("let's work on permafrost", {"QUORUM_HOOK": "off"})
        self.toggle("on")
        self.hook("let's work on permafrost")
        lines = [json.loads(l) for l in
                 (self.vault / ".quorum" / "hook.log").read_text().splitlines()]
        self.assertEqual([(l["mode"], len(l["hits"]) > 0) for l in lines],
                         [("off", False), ("off", False), ("on", True)])

    def test_every_call_is_logged_for_the_readout(self):
        self.hook("let's work on permafrost")
        self.hook("push")
        lines = [json.loads(l) for l in
                 (self.vault / ".quorum" / "hook.log").read_text().splitlines()]
        self.assertEqual([(l["session"], len(l["hits"]) > 0) for l in lines],
                         [("sess-1", True), ("sess-1", False)])
        self.assertNotIn("prompt", lines[0])


class HookReportCase(unittest.TestCase):
    """`quorum hook --report`: was the hook used, and were its hits read?"""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.home = Path(self.tmp.name) / "home"
        self.vault = self.home / "vault"
        note(self.vault / "A.md", "a", title="A")
        munged = "".join("-" if c in "/." else c for c in str(self.vault))
        tdir = self.home / ".claude" / "projects" / munged
        tdir.mkdir(parents=True)
        (self.vault / ".quorum").mkdir()
        log = [
            {"ts": "2026-10-17T01:00:00Z", "session": "s1", "terms": 4,
             "hits": ["vault:A.md", "memory:m.md"], "chars": 300},
            {"ts": "2026-10-17T01:05:00Z", "session": "s1", "terms": 1, "hits": [], "chars": 0},
            {"ts": "2026-10-17T02:00:00Z", "session": "s2", "terms": 3,
             "hits": ["vault:A.md"], "chars": 200},
        ]
        (self.vault / ".quorum" / "hook.log").write_text(
            "".join(json.dumps(l) + "\n" for l in log))

        def tool(ts, name, inp):
            return json.dumps({"type": "assistant", "timestamp": ts, "message": {
                "content": [{"type": "tool_use", "name": name, "input": inp}]}})
        (tdir / "s1.jsonl").write_text("\n".join([
            tool("2026-10-17T01:00:05Z", "Read", {"file_path": str(self.vault / "A.md")}),
            tool("2026-10-17T01:06:00Z", "Bash", {"command": "quorum q lease"}),
            tool("2026-10-17T01:07:00Z", "Bash", {"command": "quorum sym renew --all"}),
        ]) + "\n")
        (tdir / "s2.jsonl").write_text(
            tool("2026-10-17T02:00:09Z", "Grep", {"pattern": "A"}) + "\n")

    def tearDown(self):
        self.tmp.cleanup()

    def report(self, *extra):
        env = dict(os.environ, HOME=str(self.home), QUORUM_VAULT=str(self.vault),
                   QUORUM_MEMORY=str(self.home / "mem"))
        p = subprocess.run([sys.executable, str(HERE / "quorum_cli.py"), "hook", "--report",
                            "--json"] + list(extra), env=env, capture_output=True, text=True,
                           stdin=subprocess.DEVNULL)
        self.assertEqual(p.returncode, 0, p.stderr)
        return json.loads(p.stdout)

    def test_since_counts_only_the_calls_and_lookups_from_then_on(self):
        """A readout of a changed hook reads only what came after the change."""
        r = self.report("--since", "2026-10-17T01:01:00Z")
        self.assertEqual((r["calls"], r["injected"], r["hits"], r["hits_read"]), (2, 1, 1, 0))
        self.assertEqual((r["sessions"], r["lookup_calls"]), (2, 2))
        r = self.report("--since", "2026-10-17T01:30:00Z")
        self.assertEqual((r["calls"], r["sessions"], r["lookup_calls"], r["since"]),
                         (1, 1, 0, "2026-10-17T02:00:00Z"))

    def test_counts_calls_injections_and_hits_read(self):
        r = self.report()
        self.assertEqual((r["calls"], r["injected"], r["hits"], r["hits_read"]), (3, 2, 3, 1))

    def test_report_splits_sessions_by_mode_with_their_main_context_per_turn(self):
        """A/B: s1 ran with the hook on, s3 with it off; each mode reports its
        sessions and the measured main-session context per turn."""
        tdir = next((self.home / ".claude" / "projects").iterdir())
        with open(self.vault / ".quorum" / "hook.log", "a") as f:
            f.write(json.dumps({"ts": "2026-10-17T03:00:00Z", "session": "s3", "mode": "off",
                                "terms": 0, "hits": [], "chars": 0}) + "\n")

        def usage(mid, ts, cw, cr):
            return json.dumps({"type": "assistant", "timestamp": ts, "message": {
                "id": mid, "model": "claude-opus-5-5", "usage": {
                    "input_tokens": 0, "output_tokens": 0,
                    "cache_creation_input_tokens": cw, "cache_read_input_tokens": cr}}})
        with open(tdir / "s1.jsonl", "a") as f:
            f.write(usage("m1", "2026-10-17T01:00:01Z", 1000, 99000) + "\n")
        (tdir / "s3.jsonl").write_text(usage("m3", "2026-10-17T03:00:01Z", 0, 50000) + "\n")
        m = self.report()["by_mode"]
        self.assertEqual((m["on"]["sessions"], m["off"]["sessions"]), (2, 1))
        self.assertEqual((m["on"]["context_per_turn"], m["off"]["context_per_turn"]),
                         (100000, 50000))

    def test_counts_sessions_that_called_quorum_lookups(self):
        r = self.report()
        self.assertEqual((r["sessions"], r["sessions_with_lookups"], r["lookup_calls"]), (2, 1, 2))


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
