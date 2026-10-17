#!/usr/bin/env python3
"""Tests for radio_projects.py — Radio's projects file in the vault (QR-24, QR-25)."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import radio_projects as rp  # noqa: E402


def git(cwd, *args):
    p = subprocess.run(["git", "-C", str(cwd)] + list(args), capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError(p.stderr)
    return p.stdout


BASE = {"v": 1, "note": "n", "projects": {
    "sidebar": {"name": "Sidebar", "notes": ["01_Projects/Sidebar/13 - *.md"], "folders": ["01_Projects/Sidebar"],
                "repos": ["sidebar"], "concept": ["01_Projects/Sidebar/00 - *.md"]},
    "bastion": {"name": "Bastion", "notes": ["01_Projects/Bastion/21 - *.md"], "folders": ["01_Projects/Bastion"],
                "repos": [], "concept": [], "active": False}}}


class ProjectsCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.vault = root / "vault"
        self.nonvis = root / "nonvis"
        for p in ("06_Metadata", "01_Projects/Sidebar", "01_Projects/Bastion", "01_Projects/Envoy"):
            (self.vault / p).mkdir(parents=True)
        (self.vault / "01_Projects/Sidebar/13 - Briefing.md").write_text("# b")
        (self.vault / "01_Projects/Sidebar/00 - Concept.md").write_text("# c")
        (self.vault / "01_Projects/Envoy/05 - Briefing.md").write_text("# b")
        (self.vault / "01_Projects/Envoy/00 - Envoy Overview.md").write_text("# c")
        (self.vault / rp.ALLOWLIST).write_text("# allowlist\nsidebar\n")
        (self.vault / rp.PROJECTS_REL).write_text(json.dumps(BASE, indent=2))
        git(self.vault, "init", "-q", "-b", "main")
        git(self.vault, "-c", "user.name=t", "-c", "user.email=t@t", "add", "-A")
        git(self.vault, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "init")
        self.repo("envoy", "https://github.com/nonvis/envoy.git")
        self.repo("fluid", "git@github.com:fluid-protocol/fluid.git")
        self.roots = [self.nonvis]

    def tearDown(self):
        self.tmp.cleanup()

    def repo(self, name, url):
        r = self.nonvis / name
        r.mkdir(parents=True)
        git(r, "init", "-q")
        git(r, "remote", "add", "origin", url)

    def envoy(self, **kw):
        s = {"id": "envoy", "name": "Envoy", "notes": ["01_Projects/Envoy/05 - *.md"], "folders": ["01_Projects/Envoy"],
             "repos": ["envoy"], "concept": ["01_Projects/Envoy/00 - *.md"]}
        s.update(kw)
        return s

    def test_load_marks_active_by_default(self):
        pr = rp.load(self.vault)
        self.assertTrue(pr["sidebar"]["active"])
        self.assertFalse(pr["bastion"]["active"])
        self.assertEqual(sorted(rp.load(self.vault, active_only=True)), ["sidebar"])

    def test_a_missing_file_says_where_it_belongs(self):
        (self.vault / rp.PROJECTS_REL).unlink()
        with self.assertRaises(rp.ProjectsError) as e:
            rp.load(self.vault)
        self.assertIn("06_Metadata/quorum-projects.json", str(e.exception))

    def test_check_resolves_every_path(self):
        c = rp.check(self.envoy(), self.vault, self.roots)
        self.assertEqual(c["notes"][0]["matches"], ["01_Projects/Envoy/05 - Briefing.md"])
        self.assertTrue(c["folders"][0]["exists"])
        self.assertTrue(c["repos"][0]["own"])
        self.assertEqual(c["errors"], [])

    def test_check_refuses_a_repo_that_is_not_his_own(self):
        """#99: own github.com/nonvis repos only, checked from the remote URL."""
        c = rp.check(self.envoy(repos=["fluid"]), self.vault, self.roots)
        self.assertFalse(c["repos"][0]["own"])
        self.assertTrue(any("fluid" in e and "#99" in e for e in c["errors"]))

    def test_check_flags_a_missing_repo_folder_or_note(self):
        c = rp.check(self.envoy(repos=["nope"], folders=["01_Projects/Nope"], notes=["01_Projects/Envoy/99 - *.md"]),
                     self.vault, self.roots)
        self.assertEqual(len(c["errors"]), 3)

    def test_check_refuses_a_bad_id_and_a_duplicate(self):
        self.assertTrue(rp.check(self.envoy(id="Envoy X"), self.vault, self.roots)["errors"])
        self.assertTrue(rp.check(self.envoy(id="sidebar"), self.vault, self.roots, existing=rp.load(self.vault))["errors"])

    def test_check_refuses_a_path_outside_the_vault(self):
        c = rp.check(self.envoy(folders=["../etc"]), self.vault, self.roots)
        self.assertTrue(c["errors"])

    def test_add_writes_the_file_extends_the_allowlist_and_commits_both(self):
        r = rp.add(self.vault, self.envoy(), self.roots)
        self.assertTrue(r["committed"])
        pr = rp.load(self.vault)
        self.assertTrue(pr["envoy"]["active"])
        self.assertEqual(pr["envoy"]["repos"], ["envoy"])
        self.assertIn("envoy", (self.vault / rp.ALLOWLIST).read_text().split())
        self.assertEqual(git(self.vault, "status", "--porcelain").strip(), "")
        self.assertIn("envoy", git(self.vault, "log", "-1", "--format=%s"))
        self.assertFalse(r["pushed"])                     # no remote in the fixture: reported, not raised
        self.assertTrue(r["push_error"])

    def test_add_with_errors_writes_nothing(self):
        with self.assertRaises(rp.ProjectsError):
            rp.add(self.vault, self.envoy(repos=["fluid"]), self.roots)
        self.assertNotIn("envoy", rp.load(self.vault))

    def test_add_commits_only_its_own_files(self):
        (self.vault / "01_Projects/Sidebar/13 - Briefing.md").write_text("# edited, not ours")
        rp.add(self.vault, self.envoy(), self.roots)
        self.assertIn("13 - Briefing.md", git(self.vault, "status", "--porcelain"))

    def test_deactivate_and_reactivate(self):
        rp.set_active(self.vault, "sidebar", False)
        self.assertFalse(rp.load(self.vault)["sidebar"]["active"])
        self.assertIn("deactivate sidebar", git(self.vault, "log", "-1", "--format=%s"))
        rp.set_active(self.vault, "sidebar", True)
        self.assertTrue(rp.load(self.vault)["sidebar"]["active"])
        with self.assertRaises(KeyError):
            rp.set_active(self.vault, "nope", True)

    def test_update_edits_paths_after_a_check(self):
        rp.update(self.vault, "sidebar", {"concept": []}, self.roots)
        self.assertEqual(rp.load(self.vault)["sidebar"]["concept"], [])
        with self.assertRaises(rp.ProjectsError):
            rp.update(self.vault, "sidebar", {"repos": ["fluid"]}, self.roots)


if __name__ == "__main__":
    unittest.main()
