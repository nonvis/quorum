"""Distillation harvest — fill the transcript bank on demand.

v3.5's dataset is "Docent's own grounded behavior": question in, searches,
cited answer out. Real usage banks slowly; this harvester banks fast — the
brain generates N diverse, corpus-answerable questions from the note
summaries, each runs through the real agentic loop, and every transcript
lands in the bank. Run it while strong-model credit is cheap; the banked
behavior is what a future LoRA run imitates (gated by Design Decision D11 —
its output is a local model, so it waits for a revival trigger).
"""

from __future__ import annotations

import json
import re
from pathlib import Path

import bank
import loop
import retrieval
from indexer import index_project

QUESTION_PROMPT = """\
You are preparing evaluation/training questions for a knowledge agent that
answers STRICTLY from the project notes summarized below.

Write exactly {n} distinct questions an engineer working on "{project_name}"
would genuinely ask, ALL answerable from these notes. Vary the lens: where
things live, how components connect, why decisions were made, what changed
recently, current state/gaps. Vary phrasing and length; no two questions
about the same fact.

NOTE SUMMARIES
{corpus_map}

Output ONLY a JSON array of {n} strings. No prose, no markdown fence.
"""


def generate_questions(project: Path, brain, n: int) -> list[str]:
    prompt = QUESTION_PROMPT.format(
        n=n,
        project_name=project.name,
        corpus_map=retrieval.format_map(retrieval.corpus_map(project)),
    )
    reply = brain.complete(prompt)
    m = re.search(r"\[[\s\S]*\]", reply)
    if not m:
        raise ValueError(f"no JSON array in question-generation reply: {reply[:200]}")
    questions = json.loads(m.group(0))
    if not isinstance(questions, list) or not all(isinstance(q, str) for q in questions):
        raise ValueError("question-generation reply was not a list of strings")
    return [q.strip() for q in questions if q.strip()][:n]


def harvest(project: Path, brain, n: int, max_steps: int = 6, k: int = 6, log=print) -> dict:
    project = project.resolve()
    index_project(project)

    log(f"generating {n} corpus-grounded questions…")
    questions = generate_questions(project, brain, n)
    log(f"  got {len(questions)}")

    banked = 0
    failed = 0
    for i, q in enumerate(questions, 1):
        log(f"[{i}/{len(questions)}] {q[:88]}")
        try:
            result = loop.run_agent(project, q, brain, max_steps=max_steps, k=k, log=None)
            bank.bank_record(
                project,
                mode="agentic",
                brain=brain.name,
                question=q,
                answer=result["answer"],
                steps=result["steps"],
                transcript=result["transcript"],
                origin="distill",  # generated questions, not operator ones
            )
            banked += 1
            log(f"    banked ({result['steps']} steps)")
        except Exception as e:  # a bad question must not sink the harvest
            failed += 1
            log(f"    FAILED: {str(e)[:120]}")

    stats = bank.bank_stats(project)
    log(f"harvest done: banked {banked}, failed {failed} · bank now {stats['records']} record(s)")
    return {"banked": banked, "failed": failed, **stats}
