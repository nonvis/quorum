"""Golden-question eval — how the own-agent earns trust: measured, not vibed.

The golden set is a small, hand-curated bank of questions whose answers are
already known to live in the project's own knower notes. Each question declares
two cheap, deterministic checks: a *substance* check (does the answer contain
the expected fact) and a *citation* check (does it cite the note that fact lives
in). An answer only passes when it is both correct AND grounded — the same bar a
human reviewer would hold, expressed as substrings instead of a rubric.

This is the gate the agent has to clear before it graduates from a side
experiment into a real Quorum invoker option: promotion should ride on a green
golden run against the target project, not on a good-looking demo. Output is
plain text and diffable, so a regression shows up as a FAIL line — a measurement,
not a feeling.

    python3 ownagent.py eval --project <root> [--golden FILE] [--agentic]
                             [--bank]

--bank (default OFF) harvests each graded transcript into the bank, tagged
`origin: "eval"`. Off by default on purpose: the golden questions are the SAME
every run, so banking them automatically would fill the distillation set with
duplicates of a dozen questions. The flag is the harvest switch — used when an
eval run is deliberately being mined (a new brain, a reworked corpus), not on
every regression check.
"""

from __future__ import annotations

import json
from pathlib import Path

import bank
import brains
import loop


def _load_golden(golden_path: Path) -> list[dict]:
    """Parse the .jsonl bank, skipping blanks and `//` comment lines."""
    items: list[dict] = []
    with golden_path.open(encoding="utf-8") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.strip()
            if not line or line.startswith("//"):
                continue
            try:
                items.append(json.loads(line))
            except json.JSONDecodeError as e:
                raise ValueError(f"{golden_path}:{lineno}: bad JSON — {e}") from e
    return items


def _missing_label(substr_ok: bool, cite_ok: bool) -> str:
    if not substr_ok and not cite_ok:
        return "both"
    if not substr_ok:
        return "substance"
    return "citation"


def run_eval(project, brain, golden_path, agentic=False, max_steps=6, k=6,
             bank_transcripts=False) -> bool:
    """Run every golden question through the agent and grade it.

    Returns True iff every question passed (no fails, no errors).

    bank_transcripts: harvest each transcript into the bank as `origin: "eval"`
    (the `--bank` flag). PASS and FAIL alike — a wrong grounded answer is data
    about the agent's behavior too; the grade is not in the record, so a
    consumer that wants only passes must re-grade rather than assume."""
    project = Path(project)
    golden_path = Path(golden_path)
    items = _load_golden(golden_path)
    total = len(items)

    mode = "agentic" if agentic else "single-shot"
    brain_name = getattr(brain, "name", str(brain))
    print(f"golden eval · {project.name} · {mode} · brain={brain_name} · {total} questions")
    print(f"({golden_path})\n")

    passed = failed = errors = 0
    substance_ok = citation_ok = 0
    banked = bank_failed = 0

    for item in items:
        qid = item.get("id", "?")
        question = item.get("q", "")
        expect_any = item.get("expect_any") or []
        expect_cite = item.get("expect_cite") or []

        try:
            if agentic:
                result = loop.run_agent(project, question, brain, max_steps=max_steps, k=k)
            else:
                result = loop.single_shot(project, question, brain, k=k)
        except brains.BrainError as e:
            errors += 1
            print(f"ERROR {qid} ({e})")
            continue

        answer = result.get("answer", "") or ""

        if bank_transcripts:
            # Same writer `ask` uses — one bank, one record shape, one reader.
            try:
                bank.bank_record(
                    project,
                    mode="agentic" if agentic else "single_shot",
                    brain=brain_name,
                    question=question,
                    answer=answer,
                    steps=result.get("steps", 0),
                    transcript=result.get("transcript", ""),
                    origin="eval",
                )
                banked += 1
            except OSError as e:  # banking must never sink a measurement run
                bank_failed += 1
                print(f"    (bank write failed: {e})")

        low = answer.lower()
        substr_ok = (not expect_any) or any(s.lower() in low for s in expect_any)
        cite_ok = (not expect_cite) or any(c.lower() in low for c in expect_cite)
        if substr_ok:
            substance_ok += 1
        if cite_ok:
            citation_ok += 1

        if substr_ok and cite_ok:
            passed += 1
            print(f"PASS {qid}")
        else:
            failed += 1
            print(f"FAIL {qid} (missing: {_missing_label(substr_ok, cite_ok)})")
            snippet = " ".join(answer.split())[:200]
            print(f"    {snippet}")

    print()
    print("-" * 50)
    print(f"total     {total}")
    print(f"passed    {passed}")
    print(f"failed    {failed}")
    print(f"errors    {errors}")
    print(f"substance {substance_ok}/{total} · citations {citation_ok}/{total}")
    if bank_transcripts:
        note = f" ({bank_failed} write(s) failed)" if bank_failed else ""
        print(f"banked    {banked} (origin: eval){note}")

    return passed == total
