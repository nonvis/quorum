#!/usr/bin/env python3
"""Tests for scripts/quorum_cli.py — the `quorum` command after the C++ daemon
was retired (2026-10-16). Run: python3 scripts/quorum_cli_test.py
"""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
CLI = HERE / "quorum_cli.py"


class QuorumCliCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.home = Path(self.tmp.name)
        # A stub readout that echoes the argv it was given.
        self.stub = self.home / "stub_readout.py"
        self.stub.write_text("import sys\nprint(' '.join(sys.argv[1:]))\n"
                             "sys.exit(int(__import__('os').environ.get('STUB_EXIT', '0')))\n")

    def tearDown(self):
        self.tmp.cleanup()

    def run_cli(self, args, cwd=None, extra_env=None):
        env = dict(os.environ)
        env["HOME"] = str(self.home)
        env["QUORUM_SPEND_SCRIPT"] = str(self.stub)
        env.update(extra_env or {})
        return subprocess.run([sys.executable, str(CLI)] + list(args), env=env,
                              cwd=cwd or str(self.home), capture_output=True, text=True)

    def test_spend_forwards_every_flag_with_the_resolved_project(self):
        proj = self.home / "p"
        proj.mkdir()
        p = self.run_cli(["spend", "--project", str(proj), "--session", "abc",
                          "--since", "2026-10-01T00:00Z", "--until", "2026-10-02T00:00Z",
                          "--line"])
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout.split(), ["--project", str(proj), "--since",
                                            "2026-10-01T00:00Z", "--session", "abc",
                                            "--until", "2026-10-02T00:00Z", "--line"])

    def test_spend_defaults_the_project_to_the_cwd(self):
        proj = self.home / "here"
        proj.mkdir()
        p = self.run_cli(["spend", "--session", "abc"], cwd=str(proj))
        self.assertIn("--project %s" % proj.resolve(), p.stdout)

    def test_spend_expands_a_tilde_path(self):
        (self.home / "nonvis" / "vault").mkdir(parents=True)
        p = self.run_cli(["spend", "--project", "~/nonvis/vault", "--session", "x"])
        self.assertIn("--project %s" % (self.home / "nonvis" / "vault"), p.stdout)

    def test_spend_resolves_a_project_name_with_a_quorum_dir(self):
        """projects/ before nonvis/, first root holding a .quorum/ wins."""
        (self.home / "nonvis" / "pf" / ".quorum").mkdir(parents=True)
        p = self.run_cli(["spend", "--project", "pf", "--session", "x"])
        self.assertIn("--project %s" % (self.home / "nonvis" / "pf"), p.stdout)

    def test_spend_unknown_project_name_exits_1(self):
        p = self.run_cli(["spend", "--project", "nope", "--session", "x"])
        self.assertEqual((p.returncode, "nope" in p.stderr), (1, True), p.stderr)

    def test_spend_passes_the_readout_exit_code_through(self):
        p = self.run_cli(["spend", "--session", "x"], extra_env={"STUB_EXIT": "3"})
        self.assertEqual(p.returncode, 3)

    def test_spend_json_flag_is_forwarded(self):
        p = self.run_cli(["spend", "--session", "x", "--json"])
        self.assertTrue(p.stdout.strip().endswith("--json"), p.stdout)

    def test_a_retired_subcommand_says_so_and_exits_2(self):
        for sub in ("ask", "converse", "knower", "init", "search"):
            p = self.run_cli([sub])
            self.assertEqual((p.returncode, "retired" in p.stderr), (2, True),
                             sub + p.stderr)

    def test_no_subcommand_prints_usage_and_exits_2(self):
        p = self.run_cli([])
        self.assertEqual((p.returncode, "usage" in p.stderr.lower()), (2, True))

    def test_version_names_the_python_cli(self):
        p = self.run_cli(["version"])
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("quorum", p.stdout)
        self.assertIn("python", p.stdout)

    def test_study_is_removed_naming_the_decision(self):
        p = self.run_cli(["study", "list"])
        self.assertEqual(p.returncode, 2)
        self.assertIn("#129", p.stderr)


if __name__ == "__main__":
    unittest.main()
