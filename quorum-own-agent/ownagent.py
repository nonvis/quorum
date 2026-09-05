#!/usr/bin/env python3
"""quorum-own-agent — a hand-rolled AI agent that answers questions from a
Quorum project's accumulated knowledge base. Stdlib only.

    python3 ownagent.py index  --project <root>
    python3 ownagent.py map    --project <root>
    python3 ownagent.py search --project <root> "<keywords>" [-k N]
    python3 ownagent.py ask    --project <root> "<question>" [--single-shot]
                               [--brain claude|local] [--claude-model M]
                               [--base-url URL] [--local-model M]
                               [--max-steps N] [-k N] [--quiet]
    python3 ownagent.py eval   --project <root> [--golden FILE] [--agentic]
                               [--bank]

`--brain fake` also exists on every brainy command. It is TEST-ONLY and refuses
to run unless QUORUM_OWNAGENT_FAKE_BRAIN names a file holding the canned reply
(see brains.ScriptedBrain) — the gates need a deterministic complete().

See README.md for the design (the Brain seam, the text protocol, the ladder).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import loop
import retrieval
from brains import BrainError, make_brain
from indexer import index_project


def _project(args) -> Path:
    p = Path(args.project).expanduser().resolve()
    if not (p / ".quorum").is_dir():
        sys.exit(f"error: no .quorum/ under {p} — not a Quorum project")
    return p


def _brain(args):
    return make_brain(
        args.brain,
        claude_model=args.claude_model,
        base_url=args.base_url,
        local_model=args.local_model,
    )


def cmd_index(args):
    stats = index_project(_project(args))
    print(
        f"indexed {stats['indexed']} note(s) ({stats['chunks']} chunks), "
        f"{stats['unchanged']} unchanged, {stats['removed']} removed"
    )


def cmd_map(args):
    project = _project(args)
    index_project(project)
    print(retrieval.format_map(retrieval.corpus_map(project)))


def cmd_search(args):
    project = _project(args)
    index_project(project)
    print(retrieval.format_hits(retrieval.search(project, args.query, k=args.k)))


def cmd_ask(args):
    import bank  # transcript bank — v3.5 substrate

    project = _project(args)
    brain = _brain(args)
    log = None if args.quiet else lambda s: print(s, file=sys.stderr)
    if log:
        log(f"brain: {brain.name} · project: {project.name}"
            f" · mode: {'single-shot' if args.single_shot else 'agentic'}")
    try:
        if args.single_shot:
            result = loop.single_shot(project, args.question, brain, k=args.k)
        else:
            result = loop.run_agent(
                project, args.question, brain,
                max_steps=args.max_steps, k=args.k, log=log,
            )
    except BrainError as e:
        sys.exit(f"brain error: {e}")
    if not args.no_bank:
        try:
            bank.bank_record(
                project,
                mode="single_shot" if args.single_shot else "agentic",
                brain=brain.name,
                question=args.question,
                answer=result["answer"],
                steps=result["steps"],
                transcript=result["transcript"],
            )
        except OSError as e:
            if log:
                log(f"(bank write failed: {e})")
    print(result["answer"])


def cmd_distill(args):
    import distill  # lazy — the harvest is an occasional, brain-heavy op

    project = _project(args)
    brain = _brain(args)
    try:
        distill.harvest(
            project, brain, n=args.n, max_steps=args.max_steps, k=args.k,
            log=lambda s: print(s, file=sys.stderr),
        )
    except (BrainError, ValueError) as e:
        sys.exit(f"distill error: {e}")


def cmd_bank(args):
    import bank

    stats = bank.bank_stats(_project(args))
    print(f"{stats['records']} transcript(s) in {stats['files']} file(s) — {stats['dir']}")
    if stats["by_origin"]:
        # Eval-origin records repeat the golden questions; a distillation run
        # needs to see how much of the bank is that before it trains on it.
        split = " · ".join(f"{o} {n}" for o, n in sorted(stats["by_origin"].items()))
        print(f"  by origin: {split}")


def cmd_eval(args):
    import goldeval  # lazy — eval is optional and the harness ships separately

    project = _project(args)
    brain = _brain(args)
    golden = Path(args.golden) if args.golden else Path(__file__).parent / "golden" / f"{project.name}.jsonl"
    if not golden.is_file():
        sys.exit(f"error: no golden set at {golden}")
    ok = goldeval.run_eval(
        project, brain, golden, agentic=args.agentic,
        max_steps=args.max_steps, k=args.k, bank_transcripts=args.bank,
    )
    sys.exit(0 if ok else 1)


def main():
    ap = argparse.ArgumentParser(prog="ownagent", description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p, brainy: bool):
        p.add_argument("--project", default=".", help="Quorum project root (default: cwd)")
        p.add_argument("-k", type=int, default=6, help="retrieval depth")
        if brainy:
            # "fake" is the test seam (brains.ScriptedBrain) — armed only by
            # QUORUM_OWNAGENT_FAKE_BRAIN, so listing it here cannot arm it.
            p.add_argument("--brain", choices=["claude", "local", "fake"],
                           default="claude")
            p.add_argument("--claude-model", default=None,
                           help="claude -p --model (default: your CLI's default model)")
            p.add_argument("--base-url", default="http://127.0.0.1:8080",
                           help="OpenAI-compatible local server for --brain local")
            p.add_argument("--local-model", default="local")
            p.add_argument("--max-steps", type=int, default=6)

    p = sub.add_parser("index", help="build/refresh the FTS5 index")
    common(p, brainy=False)
    p.set_defaults(fn=cmd_index)

    p = sub.add_parser("map", help="print the corpus overview")
    common(p, brainy=False)
    p.set_defaults(fn=cmd_map)

    p = sub.add_parser("search", help="debug: raw BM25 retrieval")
    p.add_argument("query")
    common(p, brainy=False)
    p.set_defaults(fn=cmd_search)

    p = sub.add_parser("ask", help="ask the agent a question")
    p.add_argument("question")
    p.add_argument("--single-shot", action="store_true",
                   help="v0 pipeline: retrieve once + synthesize once (no loop)")
    p.add_argument("--quiet", action="store_true", help="print only the answer")
    p.add_argument("--no-bank", action="store_true",
                   help="don't record this ask's transcript in the bank")
    common(p, brainy=True)
    p.set_defaults(fn=cmd_ask)

    p = sub.add_parser("distill", help="harvest N grounded transcripts into the bank")
    p.add_argument("-n", type=int, default=12, help="questions to generate + run")
    common(p, brainy=True)
    p.set_defaults(fn=cmd_distill)

    p = sub.add_parser("bank", help="show transcript-bank stats")
    common(p, brainy=False)
    p.set_defaults(fn=cmd_bank)

    p = sub.add_parser("eval", help="run the golden-question set")
    p.add_argument("--golden", default=None, help="path to golden .jsonl")
    p.add_argument("--agentic", action="store_true",
                   help="evaluate the agentic loop (default: single-shot)")
    p.add_argument("--bank", action="store_true",
                   help="harvest each eval transcript into the bank as "
                        "origin=eval (default OFF: the golden questions repeat "
                        "every run and would flood the distillation set)")
    common(p, brainy=True)
    p.set_defaults(fn=cmd_eval)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
