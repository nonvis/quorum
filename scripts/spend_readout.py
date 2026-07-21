#!/usr/bin/env python3
"""
Per-run token/$ spend readout (deterministic, read-only, no LLM).

Reports the token spend of a Quorum autopilot flight against its window budget
by summing the Claude Code transcript JSONLs — the single source of record. The
autopilot supervisor runs this at halt (`quorum spend`) and copies the total +
budget-comparison line into the morning review.

WHY the transcripts are the source of record: every project's Claude Code
sessions live under `~/.claude/projects/<munged-cwd>/*.jsonl`, and that covers
the interactive supervisor session AND its Task subagents AND the daemon-spawned
headless `claude -p` runs whose cwd is inside the project. One directory, one
sum. The daemon SQLite (`.quorum/quorum.db`) also tracks `converse` costs — we
report that as a SEPARATE labeled cross-check line, NEVER added to the transcript
sum (adding it would double-count the same converse spend).

Munge rule (<abs project path> -> transcript dir): every '/' and '.' in the
absolute project path is replaced with '-', then prefixed by
`<HOME>/.claude/projects/`.
  /Users/sangsoo/nonvis/crucible -> ~/.claude/projects/-Users-sangsoo-nonvis-crucible

Transcript record shape (verified against a real file): each assistant record is
a JSON object per line with a top-level `"timestamp"` (ISO-8601, e.g.
"2026-07-21T06:08:01.953Z"), `"type":"assistant"`, and a `"message"` object
carrying `"id"`, `"model"`, and `"usage":{input_tokens, cache_creation_input_
tokens, cache_read_input_tokens, output_tokens, ...}`. Claude Code repeats the
SAME message record across several lines (identical usage), so we dedupe by
`message.id` (keep last occurrence) and count each message ONCE.

$ estimate CAVEAT: subscription usage is not billed per token — the "EST." $
column exists ONLY to compare accumulated tokens against window_budget_usd. Rates
are per-family list prices (opus/sonnet/haiku); unknown families estimate $0.

Read-only: file reads + a read-only (mode=ro) SQLite open. Writes nothing.

Usage: spend_readout.py --project <abs path> --since <ISO8601 UTC>
                        [--until <ISO8601>] [--json]
"""
import argparse
import calendar
import json
import os
import sqlite3
import sys
from datetime import datetime, timezone
from pathlib import Path

# Per-MTok list prices, matched by SUBSTRING of the model name (lowercased).
# Verified against the claude-api pricing reference 2026-07-21: fable $10/$50,
# opus 4.8 $5/$25, sonnet 5 $3/$15, haiku 4.5 $1/$5. cache_creation is billed
# at 1.25x the input rate (5-minute TTL; the 1h TTL bills 2x — we estimate at
# the default 1.25x), cache_read at 0.1x.
RATES = {
    "fable":  {"in": 10.0, "out": 50.0},
    "opus":   {"in": 5.0,  "out": 25.0},
    "sonnet": {"in": 3.0,  "out": 15.0},
    "haiku":  {"in": 1.0,  "out": 5.0},
}
CACHE_WRITE_MULT = 1.25
CACHE_READ_MULT = 0.10

EST_CAVEAT = ("EST. — subscription usage is not billed per-token; this estimate "
              "exists only to compare against window_budget_usd.")


def munged_transcript_dir(abs_project: str) -> Path:
    """<HOME>/.claude/projects/<abs project with '/' and '.' -> '-'>."""
    home = os.environ.get("HOME", "")
    munged = "".join("-" if c in "/." else c for c in abs_project)
    return Path(home) / ".claude" / "projects" / munged


def parse_ts(s: str):
    """Parse an ISO-8601 timestamp to a NAIVE UTC datetime (or None).

    All sources are UTC (transcript 'Z', --since 'Z', SQLite datetime('now')),
    so we drop the zone + fractional seconds and compare naive-UTC throughout.
    """
    if not s:
        return None
    s = s.strip()
    if s.endswith("Z"):
        s = s[:-1]
    if "." in s:
        s = s.split(".", 1)[0]
    # Drop an explicit offset (+HH:MM / -HH:MM) that follows the time part.
    ti = s.find("T")
    if ti != -1:
        tail = s[ti:]
        for sign in ("+", "-"):
            pos = tail.find(sign)
            if pos != -1:
                s = s[:ti] + tail[:pos]
                break
    for fmt in ("%Y-%m-%dT%H:%M:%S", "%Y-%m-%dT%H:%M", "%Y-%m-%d"):
        try:
            return datetime.strptime(s, fmt)
        except ValueError:
            continue
    return None


def rate_family(model: str) -> str:
    m = (model or "").lower()
    for fam in RATES:
        if fam in m:
            return fam
    return "unknown"


def est_usd(model: str, in_tok: int, out_tok: int, cw_tok: int, cr_tok: int) -> float:
    fam = rate_family(model)
    if fam == "unknown":
        return 0.0
    r = RATES[fam]
    dollars = (
        in_tok * r["in"]
        + out_tok * r["out"]
        + cw_tok * r["in"] * CACHE_WRITE_MULT
        + cr_tok * r["in"] * CACHE_READ_MULT
    ) / 1_000_000.0
    return dollars


def read_window_budget(project: str):
    """Naive line-parse of budget.window_budget_usd from .quorum/config.yaml.

    No yaml dep: find the first line whose stripped key is 'window_budget_usd'
    and float-parse the value. Absent/unparseable -> None (comparison omitted).
    """
    cfg = Path(project) / ".quorum" / "config.yaml"
    try:
        for line in cfg.read_text().splitlines():
            if ":" not in line:
                continue
            key, val = line.split(":", 1)
            if key.strip() == "window_budget_usd":
                try:
                    return float(val.strip())
                except ValueError:
                    return None
    except OSError:
        return None
    return None


def db_cross_check(project: str, since_dt: datetime, until_dt: datetime):
    """Sum conversations.spent_usd for conversations created in the window.

    Cross-check ONLY — NEVER added to the transcript EST (that would double-count
    the same converse spend). READ-ONLY (mode=ro) open. Any error (locked/missing
    db, schema mismatch) -> (None, note) so a concurrent refresh can't crash us.

    Schema (quorum-core/src/storage/schema.h): conversations(spent_usd REAL,
    created_at TEXT DEFAULT datetime('now')). created_at is UTC "YYYY-MM-DD
    HH:MM:SS", so a fixed-width string compare against the formatted window
    bounds is chronological.
    """
    db_path = Path(project) / ".quorum" / "quorum.db"
    if not db_path.exists():
        return None, "(db cross-check unavailable: no .quorum/quorum.db)"
    since_s = since_dt.strftime("%Y-%m-%d %H:%M:%S")
    until_s = until_dt.strftime("%Y-%m-%d %H:%M:%S")
    con = None
    try:
        uri = db_path.resolve().as_uri() + "?mode=ro"
        con = sqlite3.connect(uri, uri=True, timeout=1.0)
        cur = con.execute(
            "SELECT COALESCE(SUM(spent_usd), 0) FROM conversations "
            "WHERE created_at >= ? AND created_at <= ?",
            (since_s, until_s),
        )
        val = cur.fetchone()[0]
        return float(val or 0.0), None
    except Exception:
        return None, "(db cross-check unavailable)"
    finally:
        if con is not None:
            try:
                con.close()
            except Exception:
                pass


def scan_transcripts(tdir: Path, since_dt: datetime, until_dt: datetime):
    """Sum usage per model over records with timestamp in [since, until].

    Pre-filter files by mtime >= (since - 1h slack); parse line-by-line; dedupe
    by message.id (keep last occurrence). Returns (per_model, sessions_scanned,
    lines_skipped, records_counted).
    """
    slack_epoch = calendar.timegm(since_dt.timetuple()) - 3600
    # message.id -> (model, in, out, cw, cr) — last occurrence wins.
    by_msg = {}
    sessions_scanned = 0
    lines_skipped = 0
    fallback_key = 0

    for jf in sorted(tdir.glob("*.jsonl")):
        try:
            if jf.stat().st_mtime < slack_epoch:
                continue
        except OSError:
            continue
        sessions_scanned += 1
        try:
            fh = jf.open("r", encoding="utf-8", errors="replace")
        except OSError:
            continue
        with fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    d = json.loads(line)
                except Exception:
                    lines_skipped += 1
                    continue
                if d.get("type") != "assistant":
                    continue
                msg = d.get("message") or {}
                usage = msg.get("usage")
                if not isinstance(usage, dict):
                    continue
                ts = parse_ts(d.get("timestamp", ""))
                if ts is None or ts < since_dt or ts > until_dt:
                    continue
                key = msg.get("id") or d.get("requestId")
                if not key:
                    fallback_key += 1
                    key = "__nokey_%d" % fallback_key
                by_msg[key] = (
                    msg.get("model", "unknown"),
                    int(usage.get("input_tokens") or 0),
                    int(usage.get("output_tokens") or 0),
                    int(usage.get("cache_creation_input_tokens") or 0),
                    int(usage.get("cache_read_input_tokens") or 0),
                )

    per_model = {}
    for model, i, o, cw, cr in by_msg.values():
        m = per_model.setdefault(
            model,
            {"input_tokens": 0, "output_tokens": 0,
             "cache_creation_input_tokens": 0, "cache_read_input_tokens": 0},
        )
        m["input_tokens"] += i
        m["output_tokens"] += o
        m["cache_creation_input_tokens"] += cw
        m["cache_read_input_tokens"] += cr

    return per_model, sessions_scanned, lines_skipped, len(by_msg)


def build_result(project, tdir, since_dt, until_dt, since_str, until_str):
    per_model, sessions, skipped, counted = scan_transcripts(tdir, since_dt, until_dt)

    models = []
    tot = {"input_tokens": 0, "output_tokens": 0,
           "cache_creation_input_tokens": 0, "cache_read_input_tokens": 0,
           "est_usd": 0.0}
    for model in sorted(per_model):
        m = per_model[model]
        e = est_usd(model, m["input_tokens"], m["output_tokens"],
                    m["cache_creation_input_tokens"], m["cache_read_input_tokens"])
        models.append({
            "model": model,
            "family": rate_family(model),
            "input_tokens": m["input_tokens"],
            "output_tokens": m["output_tokens"],
            "cache_creation_input_tokens": m["cache_creation_input_tokens"],
            "cache_read_input_tokens": m["cache_read_input_tokens"],
            "est_usd": round(e, 4),
        })
        for k in ("input_tokens", "output_tokens",
                  "cache_creation_input_tokens", "cache_read_input_tokens"):
            tot[k] += m[k]
        tot["est_usd"] += e
    tot["est_usd"] = round(tot["est_usd"], 4)

    budget = read_window_budget(project)
    budget_pct = None
    if budget and budget > 0:
        budget_pct = round(100.0 * tot["est_usd"] / budget, 1)

    db_usd, db_note = db_cross_check(project, since_dt, until_dt)

    return {
        "project": project,
        "transcript_dir": str(tdir),
        "since": since_str,
        "until": until_str,
        "models": models,
        "total": tot,
        "window_budget_usd": budget,
        "budget_pct_est": budget_pct,
        "db_cross_check_usd": (round(db_usd, 4) if db_usd is not None else None),
        "db_cross_check_note": db_note,
        "sessions_scanned": sessions,
        "lines_skipped": skipped,
        "records_counted": counted,
        "est_caveat": EST_CAVEAT,
    }


def zeroed_result(project, tdir, since_str, until_str):
    return {
        "project": project,
        "transcript_dir": str(tdir),
        "since": since_str,
        "until": until_str,
        "models": [],
        "total": {"input_tokens": 0, "output_tokens": 0,
                  "cache_creation_input_tokens": 0, "cache_read_input_tokens": 0,
                  "est_usd": 0.0},
        "window_budget_usd": read_window_budget(project),
        "budget_pct_est": None,
        "db_cross_check_usd": None,
        "db_cross_check_note": "(no transcripts)",
        "sessions_scanned": 0,
        "lines_skipped": 0,
        "records_counted": 0,
        "est_caveat": EST_CAVEAT,
    }


def print_human(r, no_transcripts=False):
    def n(x):
        return "{:,}".format(x)

    print("Spend readout — %s" % r["project"])
    print("  window: %s -> %s" % (r["since"], r["until"]))
    print("  transcripts: %s" % r["transcript_dir"])
    if no_transcripts:
        print("  no transcripts found for %s (nothing spent, or a fresh box)"
              % r["transcript_dir"])
    print("")
    print("  %-22s %12s %10s %12s %12s %10s"
          % ("model", "in", "out", "cache-w", "cache-r", "EST $"))
    for m in r["models"]:
        print("  %-22s %12s %10s %12s %12s %9s"
              % (m["model"][:22], n(m["input_tokens"]), n(m["output_tokens"]),
                 n(m["cache_creation_input_tokens"]),
                 n(m["cache_read_input_tokens"]),
                 "$%.2f" % m["est_usd"]))
    t = r["total"]
    print("  " + "-" * 80)
    print("  %-22s %12s %10s %12s %12s %9s"
          % ("TOTAL", n(t["input_tokens"]), n(t["output_tokens"]),
             n(t["cache_creation_input_tokens"]),
             n(t["cache_read_input_tokens"]), "$%.2f" % t["est_usd"]))
    print("")
    if r["window_budget_usd"] is not None:
        pct = ("%.1f%%" % r["budget_pct_est"]
               if r["budget_pct_est"] is not None else "n/a")
        print("  window_budget_usd: $%.2f  (EST spend $%.2f = %s of budget)"
              % (r["window_budget_usd"], t["est_usd"], pct))
    if r["db_cross_check_usd"] is not None:
        print("  db cross-check (conversations.spent_usd, created_at in window): "
              "$%.2f  [separate — NOT added to the transcript EST]"
              % r["db_cross_check_usd"])
    elif r["db_cross_check_note"]:
        print("  db cross-check: %s" % r["db_cross_check_note"])
    print("  sessions scanned: %d   lines skipped (unparseable): %d"
          % (r["sessions_scanned"], r["lines_skipped"]))
    print("")
    print("  " + r["est_caveat"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--project", required=True, help="absolute project root")
    ap.add_argument("--since", required=True, help="window start (ISO8601 UTC)")
    ap.add_argument("--until", default=None, help="window end (ISO8601 UTC); default now")
    ap.add_argument("--json", action="store_true", help="emit one JSON object")
    args = ap.parse_args()

    project = str(Path(args.project))
    since_dt = parse_ts(args.since)
    if since_dt is None:
        print("ERROR: could not parse --since %r as ISO8601" % args.since,
              file=sys.stderr)
        return 2
    if args.until:
        until_dt = parse_ts(args.until)
        if until_dt is None:
            print("ERROR: could not parse --until %r as ISO8601" % args.until,
                  file=sys.stderr)
            return 2
        until_str = args.until
    else:
        until_dt = datetime.now(timezone.utc).replace(tzinfo=None)
        until_str = until_dt.strftime("%Y-%m-%dT%H:%M:%SZ")

    tdir = munged_transcript_dir(project)

    if not tdir.is_dir():
        r = zeroed_result(project, tdir, args.since, until_str)
        if args.json:
            print(json.dumps(r, indent=2))
        else:
            print_human(r, no_transcripts=True)
        return 0

    r = build_result(project, tdir, since_dt, until_dt, args.since, until_str)
    if args.json:
        print(json.dumps(r, indent=2))
    else:
        print_human(r)
    return 0


if __name__ == "__main__":
    sys.exit(main())
