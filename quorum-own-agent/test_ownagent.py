#!/usr/bin/env python3
"""
Deterministic gates for quorum-own-agent (stdlib `unittest` only, no deps).

NO REAL MODEL IS EVER CALLED. Two seams make that true:
  * `--brain fake` (brains.ScriptedBrain) replays a canned reply from a file
    named by QUORUM_OWNAGENT_FAKE_BRAIN — so the eval cases measure the
    HARNESS (banking, tagging, counting) and not a model's mood.
  * a fake `claude` executable placed FIRST on PATH — so the child-lifetime
    cases measure process handling and not the CLI.

Two properties under test:

  P3 · `eval --bank` is OPT-IN. The golden questions repeat verbatim every run,
       so automatic harvesting would fill the distillation set with duplicates.
       Off => the bank does not grow. On => one record per question, tagged
       `origin: "eval"`, countable in `bank` stats.

  P4 · the Docent kills its own `claude -p` child. Before 2026-09-04 the web
       server's timeout killed `python3 ownagent.py` and the grandchild kept
       running — an orphan burning a subscription slot for an answer nobody
       would read. SIGTERM must now take the child down and exit 143, and a
       NORMAL call must leave no handler and no child handle behind (a trap the
       agent forgets to remove hijacks its caller's own SIGTERM).

ONE assertion per test method — unittest stops a method at its first failure,
so bundled assertions cannot be owned separately by a mutation.

Run manually: python3 quorum-own-agent/test_ownagent.py   (exit 0 = pass)
"""
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
OWNAGENT = HERE / "ownagent.py"
PGREP = "/usr/bin/pgrep"

sys.path.insert(0, str(HERE))
import bank  # noqa: E402  (after the path insert, like ownagent.py's own imports)
import brains  # noqa: E402

# The canned reply the fake brain returns for EVERY call. Grounded on purpose:
# it satisfies both golden checks (substance "ledger", citation "a.md"), so an
# eval run passes and the banking path under test is the only variable.
CANNED_ANSWER = (
    "ANSWER: The sweep ledger lives in a.md — see "
    ".quorum/vaults/x/knowledge/a.md § Ledger.\n"
)

GOLDEN = [
    {"id": "g1", "q": "Where does the sweep ledger live?",
     "expect_any": ["ledger"], "expect_cite": ["a.md"]},
    {"id": "g2", "q": "Which note documents the ledger?",
     "expect_any": ["ledger"], "expect_cite": ["a.md"]},
]

NOTE_A = """---
title: Ledger
tags: [ledger, sweep]
summary: Where the sweep ledger lives and what it records.
---

# Ledger

The sweep ledger records every sweep the factory performs.
"""

NOTE_B = """---
title: Factory
tags: [factory]
summary: The registry factory and how it authorizes a sweep.
---

# Factory

The registry factory authorizes a customer sweep.
"""


class OwnAgentCase(unittest.TestCase):

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="ownagent_gate_"))
        # A tiny but REAL Quorum project: the indexer/retrieval path runs for
        # real, only the brain is faked.
        self.proj = self.tmp / "proj"
        kdir = self.proj / ".quorum" / "vaults" / "x" / "knowledge"
        kdir.mkdir(parents=True)
        (kdir / "a.md").write_text(NOTE_A)
        (kdir / "b.md").write_text(NOTE_B)

        self.golden = self.tmp / "golden.jsonl"
        self.golden.write_text("\n".join(json.dumps(g) for g in GOLDEN) + "\n")

        self.reply = self.tmp / "canned_reply.txt"
        self.reply.write_text(CANNED_ANSWER)

        self.bin = self.tmp / "bin"
        self.bin.mkdir()

    def tearDown(self):
        self._reap_fake_claude()
        shutil.rmtree(self.tmp, ignore_errors=True)

    # -- helpers ---------------------------------------------------------

    def env_with_fake_brain(self):
        env = dict(os.environ)
        env[brains.FAKE_BRAIN_ENV] = str(self.reply)
        return env

    def run_eval(self, extra=()):
        p = subprocess.run(
            [sys.executable, str(OWNAGENT), "eval",
             "--project", str(self.proj), "--golden", str(self.golden),
             "--brain", "fake"] + list(extra),
            env=self.env_with_fake_brain(), capture_output=True, text=True)
        return p

    def bank_records(self):
        d = bank.bank_dir(self.proj)
        out = []
        for f in sorted(d.glob("*.jsonl")) if d.is_dir() else []:
            for line in f.read_text(encoding="utf-8").splitlines():
                if line.strip():
                    out.append(json.loads(line))
        return out

    # ---- the fake `claude` on PATH -------------------------------------

    def write_fake_claude(self, body):
        """A fake `claude` FIRST on PATH. Python, not shell: a shell script that
        ran `sleep 30` would leave the sleep as a grandchild we could not see
        being killed — the whole point of the case."""
        exe = self.bin / "claude"
        exe.write_text("#!%s\n%s" % (sys.executable, body))
        exe.chmod(0o755)
        return exe

    def env_with_fake_claude(self):
        env = dict(os.environ)
        env["PATH"] = "%s:%s" % (self.bin, env.get("PATH", ""))
        return env

    def fake_claude_pids(self):
        p = subprocess.run([PGREP, "-f", str(self.bin)],
                           capture_output=True, text=True)
        return [int(x) for x in p.stdout.split() if x.strip()]

    def _reap_fake_claude(self):
        """Leave no orphan behind — including on a RED run, which is exactly the
        run where the child does NOT get cleaned up by the code under test."""
        for pid in self.fake_claude_pids():
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass

    def wait_for(self, pred, limit=3.0, step=0.05):
        deadline = time.monotonic() + limit
        while time.monotonic() < deadline:
            if pred():
                return True
            time.sleep(step)
        return False

    def sigterm_run(self):
        """Spawn `ownagent.py ask` against a fake `claude` that sleeps, SIGTERM
        the python, and report what happened. Returns
        {saw_child, returncode, child_gone}."""
        self.write_fake_claude("import time\ntime.sleep(30)\n")
        py = subprocess.Popen(
            [sys.executable, str(OWNAGENT), "ask", "--project", str(self.proj),
             "--quiet", "--no-bank", "where is the ledger?"],
            env=self.env_with_fake_claude(),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            saw_child = self.wait_for(lambda: bool(self.fake_claude_pids()))
            self.assertTrue(saw_child, "the fake `claude` never started — the "
                                       "case cannot measure what it claims")
            py.send_signal(signal.SIGTERM)
            try:
                rc = py.wait(timeout=3)
            except subprocess.TimeoutExpired:
                rc = None
            child_gone = self.wait_for(lambda: not self.fake_claude_pids())
        finally:
            if py.poll() is None:
                py.kill()
                py.wait(timeout=3)
            for stream in (py.stdout, py.stderr):
                try:
                    stream.close()
                except OSError:
                    pass
            self._reap_fake_claude()
        return {"saw_child": saw_child, "returncode": rc,
                "child_gone": child_gone}

    # == P3. eval --bank is opt-in =======================================

    def test_eval_without_bank_writes_nothing(self):
        """The default: a regression run must not grow the distillation set."""
        p = self.run_eval()
        self.assertEqual(self.bank_records(), [],
                         "eval banked without --bank\n" + p.stdout + p.stderr)

    def test_eval_without_bank_still_runs_the_questions(self):
        """LIVENESS for the refusal above: nothing banked, but both questions
        were really asked and graded — not silently skipped."""
        p = self.run_eval()
        self.assertIn("passed    2", p.stdout, p.stdout + p.stderr)

    def test_eval_with_bank_writes_one_record_per_question(self):
        p = self.run_eval(["--bank"])
        self.assertEqual(len(self.bank_records()), 2, p.stdout + p.stderr)

    def test_eval_banked_records_are_tagged_eval(self):
        """The tag is the whole point: distill has to be able to exclude these."""
        self.run_eval(["--bank"])
        self.assertEqual([r.get("origin") for r in self.bank_records()],
                         ["eval", "eval"])

    def test_eval_banked_records_carry_the_transcript(self):
        """A record with no transcript is not training data."""
        self.run_eval(["--bank"])
        self.assertTrue(all(r.get("transcript") for r in self.bank_records()))

    def test_bank_stats_counts_the_eval_records(self):
        """`bank` has to show the split, or the flood is invisible until it is
        in the training set."""
        self.run_eval(["--bank"])
        p = subprocess.run(
            [sys.executable, str(OWNAGENT), "bank", "--project", str(self.proj)],
            capture_output=True, text=True)
        self.assertIn("by origin: eval 2", p.stdout, p.stdout + p.stderr)

    def test_ask_records_are_not_tagged_eval(self):
        """The tag's liveness partner: an `ask` through the same writer is
        origin=ask, so the split means something."""
        bank.bank_record(self.proj, mode="agentic", brain="fake",
                         question="q", answer="a", steps=1, transcript="t")
        self.assertEqual([r.get("origin") for r in self.bank_records()], ["ask"])

    def test_fake_brain_refuses_without_its_env_var(self):
        """The test seam must not be armable by a flag alone."""
        env = dict(os.environ)
        env.pop(brains.FAKE_BRAIN_ENV, None)
        p = subprocess.run(
            [sys.executable, str(OWNAGENT), "eval", "--project", str(self.proj),
             "--golden", str(self.golden), "--brain", "fake"],
            env=env, capture_output=True, text=True)
        self.assertIn("test-only", p.stdout + p.stderr)

    # == P4. the child dies with us ======================================

    def test_sigterm_exits_the_agent(self):
        self.assertIsNotNone(self.sigterm_run()["returncode"],
                             "ownagent did not exit within 3 s of SIGTERM")

    def test_sigterm_exit_status_is_143(self):
        """128+SIGTERM — the conventional status, and evidence the HANDLER ran
        rather than the default disposition killing us."""
        self.assertEqual(self.sigterm_run()["returncode"], 143)

    def test_sigterm_kills_the_claude_child(self):
        """The bug itself: python died, `claude -p` kept running."""
        self.assertTrue(self.sigterm_run()["child_gone"],
                        "the fake `claude` outlived the agent — orphaned child")

    # -- and the liveness side: a NORMAL call leaves nothing behind -------

    def completed_call(self):
        """Run one real ClaudeCLIBrain.complete() against a fake claude that
        answers immediately, in THIS process, with a sentinel handler installed
        so restoration is observable."""
        self.write_fake_claude("print('ANSWER: ok')\n")
        sentinel = lambda signum, frame: None  # noqa: E731
        old_path = os.environ.get("PATH", "")
        old_term = signal.signal(signal.SIGTERM, sentinel)
        old_int = signal.signal(signal.SIGINT, sentinel)
        try:
            os.environ["PATH"] = "%s:%s" % (self.bin, old_path)
            out = brains.ClaudeCLIBrain(timeout=30).complete("hello")
            self.assertEqual(out, "ANSWER: ok", "the fake claude did not answer")
            return {"sentinel": sentinel,
                    "term": signal.getsignal(signal.SIGTERM),
                    "int": signal.getsignal(signal.SIGINT),
                    "child": brains._ACTIVE_CHILD}
        finally:
            os.environ["PATH"] = old_path
            signal.signal(signal.SIGTERM, old_term)
            signal.signal(signal.SIGINT, old_int)

    def test_completed_call_restores_the_sigterm_handler(self):
        r = self.completed_call()
        self.assertIs(r["term"], r["sentinel"],
                      "a handler left installed hijacks the caller's SIGTERM")

    def test_completed_call_restores_the_sigint_handler(self):
        r = self.completed_call()
        self.assertIs(r["int"], r["sentinel"])

    def test_completed_call_clears_the_child_handle(self):
        """A stale handle means the NEXT signal terminates a dead pid and the
        real child survives."""
        self.assertIsNone(self.completed_call()["child"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
