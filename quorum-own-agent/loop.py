"""The agent loop — a hand-rolled ReAct cycle over the text protocol.

    prompt -> brain -> (THINK/ACTION | ANSWER)
                 ^          |
                 |       execute tool -> OBSERVATION appended
                 +----------+

No framework, no native tool-calling API: the loop parses ACTION lines out of
plain text, executes the tool, appends the observation, and calls the brain
again on the growing transcript. That keeps the harness brain-agnostic and
makes every mechanism visible (parsing, malformed-reply nudges, step budget,
forced final answer).

`single_shot` is the v0 pipeline (retrieve once, synthesize once) — cheaper,
and the baseline the agentic mode has to beat in eval.
"""

from __future__ import annotations

import re
from pathlib import Path

import prompts
import retrieval
from indexer import index_project

_ACTION_RE = re.compile(
    r"^\s*ACTION:\s*(search|read|map)\s*\(\s*(?:\"([^\"]*)\"|'([^']*)'|)\s*\)\s*$",
    re.M,
)
_ANSWER_RE = re.compile(r"^\s*ANSWER:\s*", re.M)


def _first_directive(reply: str):
    """Return ("action", tool, arg) or ("answer", text) — whichever appears
    first in the reply — or None if the protocol wasn't followed."""
    am = _ACTION_RE.search(reply)
    ansm = _ANSWER_RE.search(reply)
    if am and (not ansm or am.start() < ansm.start()):
        return ("action", am.group(1), am.group(2) or am.group(3) or "")
    if ansm:
        return ("answer", reply[ansm.end() :].strip())
    return None


def _execute(project: Path, tool: str, arg: str, k: int) -> str:
    if tool == "search":
        return retrieval.format_hits(retrieval.search(project, arg, k=k))
    if tool == "read":
        try:
            return retrieval.read_note(project, arg.strip())
        except (FileNotFoundError, PermissionError) as e:
            return f"(error: {e})"
    if tool == "map":
        return retrieval.format_map(retrieval.corpus_map(project))
    return f"(error: unknown tool {tool})"


def run_agent(
    project: Path,
    question: str,
    brain,
    max_steps: int = 6,
    k: int = 6,
    log=None,
) -> dict:
    """The agentic mode. Returns {answer, steps, transcript}."""
    project = project.resolve()
    index_project(project)  # incremental — keeps the index fresh for free

    transcript = prompts.AGENT_PREAMBLE.format(
        project_name=project.name,
        max_steps=max_steps,
        corpus_map=retrieval.format_map(retrieval.corpus_map(project)),
        question=question,
    )

    steps = 0
    for _ in range(max_steps):
        steps += 1
        reply = brain.complete(transcript)
        directive = _first_directive(reply)

        if directive is None:
            if log:
                log(f"  [step {steps}] malformed reply — nudging")
            transcript += f"\n\n{reply}\n{prompts.NUDGE}"
            continue

        if directive[0] == "answer":
            if log:
                log(f"  [step {steps}] ANSWER")
            return {"answer": directive[1], "steps": steps, "transcript": transcript + "\n\n" + reply}

        _, tool, arg = directive
        if log:
            log(f"  [step {steps}] ACTION: {tool}({arg!r})")
        observation = _execute(project, tool, arg, k)
        transcript += f"\n\n{reply}\n" + prompts.OBSERVATION_TEMPLATE.format(
            tool=f"{tool}({arg})", observation=observation
        )

    # Budget exhausted — force a grounded final answer.
    transcript += prompts.FORCE_ANSWER
    reply = brain.complete(transcript)
    directive = _first_directive(reply)
    answer = directive[1] if directive and directive[0] == "answer" else reply.strip()
    if log:
        log(f"  [step {steps + 1}] forced ANSWER")
    return {"answer": answer, "steps": steps + 1, "transcript": transcript + "\n\n" + reply}


def single_shot(project: Path, question: str, brain, k: int = 8) -> dict:
    """v0 pipeline: one retrieval pass, one synthesis call."""
    project = project.resolve()
    index_project(project)
    hits = retrieval.search(project, question, k=k)
    prompt = prompts.SINGLE_SHOT.format(
        project_name=project.name,
        context=retrieval.format_hits(hits, per_chunk=1200),
        question=question,
    )
    answer = brain.complete(prompt)
    return {
        "answer": answer,
        "hits": hits,
        "steps": len(hits),
        # same field name as run_agent so the bank stores both modes uniformly
        "transcript": prompt + "\n\n" + answer,
    }
