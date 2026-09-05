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
    origin    "ask" | "eval" | "distill" — WHERE the question came from
    brain     brain.name (which model produced the behavior)
    question  the operator's question
    answer    the final answer (with citations)
    steps     action count (agentic) or retrieval depth (single-shot)
    transcript  the FULL prompt+reply text — preamble, actions, observations

`origin` exists because the golden set repeats the SAME questions every eval
run: harvested unfiltered they would dominate a distillation set by sheer
duplication. Marking them keeps `eval --bank` optional AND separable after the
fact. Records written before 2026-09-04 carry no origin field; bank_stats counts
those as "unmarked" rather than guessing.
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
    origin: str = "ask",
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
        "origin": origin,
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
    """Record/file counts plus the per-origin split.

    by_origin buckets: the record's `origin`, "unmarked" for pre-origin records,
    "unparseable" for a line that isn't JSON. A bucket that cannot be read is
    named, not dropped into the majority one.
    """
    d = bank_dir(project)
    files = sorted(d.glob("*.jsonl")) if d.is_dir() else []
    count = 0
    by_origin: dict[str, int] = {}
    for f in files:
        for line in f.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            count += 1
            try:
                origin = json.loads(line).get("origin") or "unmarked"
            except (json.JSONDecodeError, AttributeError):
                origin = "unparseable"
            by_origin[origin] = by_origin.get(origin, 0) + 1
    return {
        "files": len(files),
        "records": count,
        "by_origin": by_origin,
        "eval_records": by_origin.get("eval", 0),
        "dir": str(d),
    }
