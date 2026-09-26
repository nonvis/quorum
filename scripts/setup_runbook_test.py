#!/usr/bin/env python3
"""SETUP.md stays the single source of truth for setting up Quorum.

Registered in ctest as test_setup_runbook. Fails when
  - SETUP.md's step IDs (`### S1 — ...`) and scripts/doctor.sh's `step S1`
    lines differ, in either direction or in order, or
  - README.md / DEVELOPMENT.md / OPERATOR.md carry their own package-install
    lines (setup steps belong in SETUP.md; those files link to it).
No build, no network, no claude.
"""

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STEP_ID = r"[SP]\d+"


def runbook_steps() -> list[str]:
    text = (ROOT / "SETUP.md").read_text()
    return re.findall(rf"^### ({STEP_ID}) — ", text, flags=re.M)


def doctor_steps() -> list[str]:
    text = (ROOT / "scripts" / "doctor.sh").read_text()
    return re.findall(rf"^\s*step ({STEP_ID})\b", text, flags=re.M)


class SetupRunbookTest(unittest.TestCase):
    def test_runbook_has_steps(self):
        self.assertTrue(runbook_steps(), "no `### S1 — ...` headings found in SETUP.md")

    def test_doctor_checks_every_runbook_step(self):
        self.assertEqual(
            runbook_steps(),
            doctor_steps(),
            "SETUP.md step headings and scripts/doctor.sh `step` lines must match "
            "one to one, in order — update both in the same commit",
        )

    def test_other_docs_do_not_repeat_setup(self):
        install_line = re.compile(r"^\s*(sudo\s+)?(apt-get|apt|brew)\s+install\b", re.M)
        for name in ("README.md", "DEVELOPMENT.md", "OPERATOR.md"):
            with self.subTest(doc=name):
                hits = install_line.findall((ROOT / name).read_text())
                self.assertFalse(
                    hits,
                    f"{name} has its own package-install lines; put setup steps in "
                    "SETUP.md and link to it",
                )


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
