"""The Brain seam — the ONE interface the rest of the agent depends on.

A Brain is just `complete(prompt) -> text`. Everything agentic (the loop, the
tool protocol, retrieval, citations) is hand-rolled around this seam, so the
model is swappable without touching the harness:

    ClaudeCLIBrain   — shells `claude -p` (subscription-authed, like the
                       daemon's invoker). Default: NO --model flag, so it
                       tracks whatever the operator's CLI default is.
    LocalServerBrain — any OpenAI-compatible local server (llama-server,
                       LM Studio, mlx_lm.server…). This is the v2 ladder rung:
                       point --base-url at it and the same agent runs $0/offline.
    ScriptedBrain    — TEST-ONLY: replays a canned reply from a file. Gated
                       behind an env var so no production path can get it.

CHILD LIFETIME (2026-09-04): `claude -p` runs as a Popen child with a
module-level handle, and SIGTERM/SIGINT are trapped for the duration of the call
so the child dies with us. Before this, the web server's timeout killed
`python3 ownagent.py` and the `claude -p` grandchild kept running — an orphan
burning a subscription slot with nobody to read its answer.
"""

from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path


class BrainError(RuntimeError):
    pass


# The live `claude -p` child, or None. Module-level because a SIGNAL HANDLER has
# to reach it: the handler cannot be passed arguments, and by the time it fires
# the stack that owns the Popen is exactly the one being torn down. Single-
# threaded by construction (one brain call at a time per process).
_ACTIVE_CHILD: subprocess.Popen | None = None


def _terminate_active_child(grace: float = 2.0) -> None:
    """SIGTERM the live child, escalate to SIGKILL after `grace` seconds."""
    proc = _ACTIVE_CHILD
    if proc is None or proc.poll() is not None:
        return
    try:
        proc.terminate()
        try:
            proc.wait(timeout=grace)
        except subprocess.TimeoutExpired:
            proc.kill()
            try:
                proc.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                pass
    except OSError:
        pass


def _on_terminating_signal(signum, frame):
    """Kill the child, then leave with the conventional 128+signum status.

    os._exit, not sys.exit: raising SystemExit from a handler unwinds through
    whatever `except` happens to be on the stack, and a swallowed exit is the
    same orphan bug one level up. Flush by hand since os._exit does not.
    """
    _terminate_active_child()
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.flush()
        except Exception:
            pass
    os._exit(128 + signum)  # SIGTERM -> 143, SIGINT -> 130


def _install_signal_handlers():
    """Trap SIGTERM/SIGINT; return a callable that puts the old ones back.

    Installed ONLY around a live child — the agent is a library as well as a
    CLI, so it must not leave a process-wide handler behind. signal.signal is
    main-thread-only; off the main thread we skip the trap (the Popen handle
    still gives the caller something to kill) rather than fail the call.
    """
    previous = {}
    for sig in (signal.SIGTERM, signal.SIGINT):
        try:
            previous[sig] = signal.signal(sig, _on_terminating_signal)
        except (ValueError, OSError):
            pass

    def restore():
        for sig, prev in previous.items():
            try:
                signal.signal(sig, prev)
            except (ValueError, OSError):
                pass

    return restore


class ClaudeCLIBrain:
    def __init__(self, model: str | None = None, timeout: int = 300):
        self.model = model
        self.timeout = timeout
        self.name = f"claude-cli({model or 'cli-default'})"

    def complete(self, prompt: str) -> str:
        global _ACTIVE_CHILD
        cmd = ["claude", "-p", prompt]
        if self.model:
            cmd += ["--model", self.model]
        env = dict(os.environ)
        env.pop("CLAUDECODE", None)  # same nesting-guard the daemon uses
        try:
            proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env=env,
            )
        except FileNotFoundError as e:
            raise BrainError("`claude` CLI not on PATH") from e
        _ACTIVE_CHILD = proc
        restore_signals = _install_signal_handlers()
        try:
            out, err = proc.communicate(timeout=self.timeout)
        except subprocess.TimeoutExpired as e:
            _terminate_active_child()
            proc.communicate()  # reap; the pipes are already closed by the kill
            raise BrainError(f"claude -p timed out after {self.timeout}s") from e
        finally:
            # Both directions matter: an uninstalled handler orphans the NEXT
            # child, a left-behind one hijacks the caller's own SIGTERM.
            restore_signals()
            _ACTIVE_CHILD = None
        if proc.returncode != 0:
            tail = (err or out).strip()[-400:]
            raise BrainError(f"claude -p exited {proc.returncode}: {tail}")
        return out.strip()


class LocalServerBrain:
    def __init__(
        self,
        base_url: str = "http://127.0.0.1:8080",
        model: str = "local",
        timeout: int = 300,
    ):
        self.base_url = base_url.rstrip("/")
        self.model = model
        self.timeout = timeout
        self.name = f"local({self.base_url}, {model})"

    def complete(self, prompt: str) -> str:
        payload = json.dumps(
            {
                "model": self.model,
                "messages": [{"role": "user", "content": prompt}],
                "temperature": 0.2,
                "max_tokens": 900,
            }
        ).encode()
        req = urllib.request.Request(
            f"{self.base_url}/v1/chat/completions",
            data=payload,
            headers={"Content-Type": "application/json"},
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                data = json.loads(resp.read())
        except (urllib.error.URLError, TimeoutError) as e:
            raise BrainError(
                f"local server unreachable at {self.base_url} — is it running?"
            ) from e
        try:
            return data["choices"][0]["message"]["content"].strip()
        except (KeyError, IndexError) as e:
            raise BrainError(f"unexpected response shape: {str(data)[:300]}") from e


# The env var that arms `--brain fake`. Named, not inferred: a stub brain that
# could be selected by a flag alone would eventually be selected by accident.
FAKE_BRAIN_ENV = "QUORUM_OWNAGENT_FAKE_BRAIN"


class ScriptedBrain:
    """TEST-ONLY brain: replays one canned reply from a file. No model, no cost.

    The harness's own gates need a `complete()` that returns a known grounded
    ANSWER so the mechanism under test (banking, signal handling, the loop) is
    what's being measured rather than a model's mood. Selected by
    `--brain fake`, which is REFUSED unless QUORUM_OWNAGENT_FAKE_BRAIN points at
    a readable file — so no production invocation can silently get a stub.

    The whole file is the reply, returned for EVERY call (the gates need a
    canned answer, not a dialogue). `.calls` counts them for assertions.
    """

    def __init__(self, path: str):
        self.path = path
        try:
            self._reply = Path(path).read_text(encoding="utf-8").strip()
        except OSError as e:
            raise BrainError(f"--brain fake: cannot read {path}: {e}") from e
        self.calls = 0
        self.name = f"fake({Path(path).name})"

    def complete(self, prompt: str) -> str:
        self.calls += 1
        return self._reply


def make_brain(
    kind: str,
    claude_model: str | None = None,
    base_url: str = "http://127.0.0.1:8080",
    local_model: str = "local",
):
    if kind == "claude":
        return ClaudeCLIBrain(model=claude_model)
    if kind == "local":
        return LocalServerBrain(base_url=base_url, model=local_model)
    if kind == "fake":
        path = os.environ.get(FAKE_BRAIN_ENV)
        if not path:
            raise BrainError(
                f"--brain fake is test-only: set {FAKE_BRAIN_ENV}=<file holding "
                "the canned reply>"
            )
        return ScriptedBrain(path)
    raise BrainError(f"unknown brain: {kind} (use 'claude', 'local' or 'fake')")
