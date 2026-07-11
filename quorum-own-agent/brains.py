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
"""

from __future__ import annotations

import json
import os
import subprocess
import urllib.error
import urllib.request


class BrainError(RuntimeError):
    pass


class ClaudeCLIBrain:
    def __init__(self, model: str | None = None, timeout: int = 300):
        self.model = model
        self.timeout = timeout
        self.name = f"claude-cli({model or 'cli-default'})"

    def complete(self, prompt: str) -> str:
        cmd = ["claude", "-p", prompt]
        if self.model:
            cmd += ["--model", self.model]
        env = dict(os.environ)
        env.pop("CLAUDECODE", None)  # same nesting-guard the daemon uses
        try:
            proc = subprocess.run(
                cmd, capture_output=True, text=True, timeout=self.timeout, env=env
            )
        except FileNotFoundError as e:
            raise BrainError("`claude` CLI not on PATH") from e
        except subprocess.TimeoutExpired as e:
            raise BrainError(f"claude -p timed out after {self.timeout}s") from e
        if proc.returncode != 0:
            tail = (proc.stderr or proc.stdout).strip()[-400:]
            raise BrainError(f"claude -p exited {proc.returncode}: {tail}")
        return proc.stdout.strip()


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
    raise BrainError(f"unknown brain: {kind} (use 'claude' or 'local')")
