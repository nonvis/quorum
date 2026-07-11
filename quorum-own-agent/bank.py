"""The transcript bank — v3.5's substrate.

Every ask (CLI and web alike) is banked as one JSONL record under the target
project's `.quorum/own-agent/transcripts/`, so a future LoRA distillation run
(fine-tune a small open model on Docent's own grounded behavior) has data the
day it's wanted. The bank is append-only, per-day files, and stays inside the
project — the transcripts quote the project's knowledge, so they live with it.

Record shape (kept raw on purpose; a training run reformats to its template):
    ts        UTC ISO-8601
    project   absolute project root
    mode      "agentic" | "single_shot"
    brain     brain.name (which model produced the behavior)
    question  the operator's question
    answer    the final answer (with citations)
    steps     action count (agentic) or retrieval depth (single-shot)
    transcript  the FULL prompt+reply text — preamble, actions, observations
"""

from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path

BANK_REL = Path(".quorum") / "own-agent" / "transcripts"


def bank_dir(project: Path) -> Path:
    return project / BANK_REL


def bank_record(
    project: Path,
    *,
    mode: str,
    brain: str,
    question: str,
    answer: str,
    steps: int,
    transcript: str,
) -> Path:
    """Append one record; returns the file it landed in. Never raises upward —
    banking must not break answering (callers wrap in try/except)."""
    now = datetime.now(timezone.utc)
    d = bank_dir(project)
    d.mkdir(parents=True, exist_ok=True)
    out = d / f"{now:%Y-%m-%d}.jsonl"
    record = {
        "ts": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "project": str(project),
        "mode": mode,
        "brain": brain,
        "question": question,
        "answer": answer,
        "steps": steps,
        "transcript": transcript,
    }
    with out.open("a", encoding="utf-8") as f:
        f.write(json.dumps(record, ensure_ascii=False) + "\n")
    return out


def bank_stats(project: Path) -> dict:
    d = bank_dir(project)
    files = sorted(d.glob("*.jsonl")) if d.is_dir() else []
    count = 0
    for f in files:
        count += sum(1 for line in f.read_text(encoding="utf-8").splitlines() if line.strip())
    return {"files": len(files), "records": count, "dir": str(d)}
