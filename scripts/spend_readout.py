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
are per-family list prices (fable/opus/sonnet/haiku); a model whose family is NOT
in RATES is priced `n/a`, NEVER $0 — its tokens are still reported and the TOTAL
is then labelled a FLOOR (absent rate != zero spend).

ABSENT != ZERO. Three distinct outcomes, three exit codes:
  * transcript dir ABSENT  -> every $ figure prints `n/a`, exit 3, JSON
    "status":"no_transcript_dir". We cannot see this source; we do not claim it
    spent nothing. The dir is absent either because no Claude Code session ever
    ran with this cwd, or because Claude Code's transcript retention pruned it.
  * transcript dir present but EMPTY (or no record in the window) -> $0.00,
    `sessions scanned: 0`, exit 0. That IS zero: we looked and there was nothing.
  * a bad --since/--until -> exit 2.

RETENTION FLOOR: Claude Code prunes transcripts older than `cleanupPeriodDays`
(<HOME>/.claude/settings.json; absent -> Claude Code's documented default of 30
days). We print the resulting horizon and WARN when --since predates it — the
window's older half may simply no longer exist on disk, so the total is a FLOOR.

Read-only: file reads + a read-only (mode=ro) SQLite open. Writes nothing.

HOME is the single injection seam for tests: it selects BOTH the transcript root
(`$HOME/.claude/projects/...`) and the settings file read for cleanupPeriodDays.

Usage: spend_readout.py --project <abs path> --since <ISO8601 UTC>
                        [--until <ISO8601>] [--json]
Exit:  0 ok · 2 bad --since/--until · 3 transcript dir absent
"""
import argparse
import calendar
import json
import os
import sqlite3
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

# Per-MTok list prices, matched by SUBSTRING of the model name (lowercased).
# Verified against the claude-api pricing reference 2026-07-21: fable $10/$50,
# opus 4.8 $5/$25, sonnet 5 $3/$15, haiku 4.5 $1/$5. cache_creation is billed
# at 1.25x the input rate (5-minute TTL; the 1h TTL bills 2x — we estimate at
# the default 1.25x), cache_read at 0.1x.
# Re-checked 2026-09-04: no local pricing reference found on this box (searched
# ~/.claude for a claude-api skill / any *pricing* file — none exists), and this
# script does not fetch the web. Values UNCHANGED and still dated 07-21; treat
# them as stale until a reference is on disk to diff against.
# A model whose family is absent here is priced n/a, never $0 — see est_usd.
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

# Claude Code's documented default transcript retention when settings.json sets
# no `cleanupPeriodDays`. Empirically consistent on 2026-09-04: the oldest jsonl
# in this box's busiest transcript dir was 08-05, exactly 30 days back.
DEFAULT_CLEANUP_PERIOD_DAYS = 30

ABSENT_NOTE = ("ABSENT (never a session with this cwd, or pruned by Claude "
               "Code's transcript retention)")


def claude_home(home: str = None) -> Path:
    """<HOME>/.claude — the ONE place HOME is resolved. Injectable for tests."""
    if home is None:
        home = os.environ.get("HOME", "")
    return Path(home) / ".claude"


def munged_transcript_dir(abs_project: str, home: str = None) -> Path:
    """<HOME>/.claude/projects/<abs project with '/' and '.' -> '-'>."""
    munged = "".join("-" if c in "/." else c for c in abs_project)
    return claude_home(home) / "projects" / munged


def retention_days(home: str = None):
    """(days, source) — settings.json `cleanupPeriodDays`, else the CC default.

    source is literally "settings.json" or "default" so the readout can say
    WHERE the number came from rather than asserting a bare horizon. Any read
    or parse failure falls back to the default (a missing setting IS the
    default; a corrupt file must not make us claim a longer horizon).
    """
    path = claude_home(home) / "settings.json"
    try:
        d = json.loads(path.read_text())
        v = d.get("cleanupPeriodDays")
        if isinstance(v, bool) or not isinstance(v, (int, float)):
            return DEFAULT_CLEANUP_PERIOD_DAYS, "default"
        n = int(v)
        if n <= 0:
            return DEFAULT_CLEANUP_PERIOD_DAYS, "default"
        return n, "settings.json"
    except Exception:
        return DEFAULT_CLEANUP_PERIOD_DAYS, "default"


def retention_info(now_dt: datetime, home: str = None):
    """{days, source, horizon} — the date before which transcripts may be gone."""
    days, source = retention_days(home)
    horizon = (now_dt - timedelta(days=days)).date().isoformat()
    return {"retention_days": days,
            "retention_source": source,
            "retention_horizon": horizon}


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


def est_usd(model: str, in_tok: int, out_tok: int, cw_tok: int, cr_tok: int):
    """Per-MTok estimate, or None when the model's family has no known rate.

    None, NOT 0.0 — a rate we do not have is not a spend of zero. The caller
    reports such a model with `n/a` and marks the TOTAL a FLOOR.
    """
    fam = rate_family(model)
    if fam == "unknown":
        return None
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


def build_result(project, tdir, since_dt, until_dt, since_str, until_str,
                 retention):
    per_model, sessions, skipped, counted = scan_transcripts(tdir, since_dt, until_dt)

    models = []
    unknown_models = []
    tot = {"input_tokens": 0, "output_tokens": 0,
           "cache_creation_input_tokens": 0, "cache_read_input_tokens": 0,
           "est_usd": 0.0}
    for model in sorted(per_model):
        m = per_model[model]
        e = est_usd(model, m["input_tokens"], m["output_tokens"],
                    m["cache_creation_input_tokens"], m["cache_read_input_tokens"])
        fam = rate_family(model)
        if fam == "unknown":
            unknown_models.append(model)
        models.append({
            "model": model,
            "family": fam,
            "input_tokens": m["input_tokens"],
            "output_tokens": m["output_tokens"],
            "cache_creation_input_tokens": m["cache_creation_input_tokens"],
            "cache_read_input_tokens": m["cache_read_input_tokens"],
            # None (JSON null) for an unknown family — never 0.0.
            "est_usd": (round(e, 4) if e is not None else None),
        })
        for k in ("input_tokens", "output_tokens",
                  "cache_creation_input_tokens", "cache_read_input_tokens"):
            tot[k] += m[k]
        if e is not None:
            tot["est_usd"] += e
    tot["est_usd"] = round(tot["est_usd"], 4)
    # The TOTAL under-counts whenever a model we saw has no rate: it is a FLOOR.
    tot["est_is_floor"] = bool(unknown_models)

    budget = read_window_budget(project)
    budget_pct = None
    if budget and budget > 0:
        budget_pct = round(100.0 * tot["est_usd"] / budget, 1)

    db_usd, db_note = db_cross_check(project, since_dt, until_dt)

    # The window reaches back past what Claude Code still keeps on disk => the
    # older part of it cannot be measured, only under-reported.
    since_predates = since_dt.date().isoformat() < retention["retention_horizon"]

    r = {
        "status": "ok",
        "project": project,
        "transcript_dir": str(tdir),
        "transcript_dir_present": True,
        "since": since_str,
        "until": until_str,
        "models": models,
        "unknown_families": unknown_models,
        "total": tot,
        "window_budget_usd": budget,
        "budget_pct_est": budget_pct,
        "db_cross_check_usd": (round(db_usd, 4) if db_usd is not None else None),
        "db_cross_check_note": db_note,
        "sessions_scanned": sessions,
        "lines_skipped": skipped,
        "records_counted": counted,
        "since_predates_retention": since_predates,
        "est_caveat": EST_CAVEAT,
    }
    r.update(retention)
    return r


def absent_result(project, tdir, since_dt, since_str, until_str, retention):
    """The transcript dir does not exist — every $ figure is UNKNOWN, not zero.

    No token counts, no est_usd, no session count: reporting 0 for any of them
    would be a measurement we did not make. `status` and the exit code carry it.
    """
    since_predates = since_dt.date().isoformat() < retention["retention_horizon"]
    r = {
        "status": "no_transcript_dir",
        "project": project,
        "transcript_dir": str(tdir),
        "transcript_dir_present": False,
        "transcript_dir_note": ABSENT_NOTE,
        "since": since_str,
        "until": until_str,
        "models": [],
        "unknown_families": [],
        "total": {"input_tokens": None, "output_tokens": None,
                  "cache_creation_input_tokens": None,
                  "cache_read_input_tokens": None,
                  "est_usd": None, "est_is_floor": None},
        "window_budget_usd": read_window_budget(project),
        "budget_pct_est": None,
        "db_cross_check_usd": None,
        "db_cross_check_note": "(transcript dir absent — cross-check not run)",
        "sessions_scanned": None,
        "lines_skipped": None,
        "records_counted": None,
        "since_predates_retention": since_predates,
        "est_caveat": EST_CAVEAT,
    }
    r.update(retention)
    return r


def print_human(r):
    absent = (r["status"] == "no_transcript_dir")

    def n(x):
        return "n/a" if x is None else "{:,}".format(x)

    def d(x):
        return "n/a" if x is None else "$%.2f" % x

    print("Spend readout — %s" % r["project"])
    print("  window: %s -> %s" % (r["since"], r["until"]))
    if absent:
        # The required shape: the path, then ABSENT and why. No $ figure here.
        print("  transcripts: %s — %s" % (r["transcript_dir"], ABSENT_NOTE))
    else:
        print("  transcripts: %s" % r["transcript_dir"])
    print("  retention horizon ≈ %s  (cleanupPeriodDays=%d, source: %s)"
          % (r["retention_horizon"], r["retention_days"], r["retention_source"]))
    if r["since_predates_retention"]:
        print("  WARNING: --since %s predates the retention horizon — sessions "
              "before %s may have been pruned — totals are a FLOOR"
              % (r["since"], r["retention_horizon"]))
    print("")
    print("  %-22s %12s %10s %12s %12s %10s"
          % ("model", "in", "out", "cache-w", "cache-r", "EST $"))
    for m in r["models"]:
        print("  %-22s %12s %10s %12s %12s %9s"
              % (m["model"][:22], n(m["input_tokens"]), n(m["output_tokens"]),
                 n(m["cache_creation_input_tokens"]),
                 n(m["cache_read_input_tokens"]),
                 d(m["est_usd"])))
    t = r["total"]
    print("  " + "-" * 80)
    total_est = d(t["est_usd"])
    if t.get("est_is_floor"):
        total_est += "+"
    print("  %-22s %12s %10s %12s %12s %9s"
          % ("TOTAL", n(t["input_tokens"]), n(t["output_tokens"]),
             n(t["cache_creation_input_tokens"]),
             n(t["cache_read_input_tokens"]), total_est))
    if t.get("est_is_floor"):
        print("  TOTAL is a FLOOR — no rate for %s; its tokens are counted, its "
              "$ is not." % ", ".join(r["unknown_families"]))
    print("")
    if absent:
        print("  This source could not be read. It is NOT a spend of $0 — carry "
              "n/a into the checkpoint, not a number.")
    if r["window_budget_usd"] is not None:
        if t["est_usd"] is None:
            print("  window_budget_usd: $%.2f  (EST spend n/a — transcript dir "
                  "absent)" % r["window_budget_usd"])
        else:
            pct = ("%.1f%%" % r["budget_pct_est"]
                   if r["budget_pct_est"] is not None else "n/a")
            floor = " — a FLOOR" if t.get("est_is_floor") else ""
            print("  window_budget_usd: $%.2f  (EST spend $%.2f = %s of budget%s)"
                  % (r["window_budget_usd"], t["est_usd"], pct, floor))
    if r["db_cross_check_usd"] is not None:
        print("  db cross-check (conversations.spent_usd, created_at in window): "
              "$%.2f  [separate — NOT added to the transcript EST]"
              % r["db_cross_check_usd"])
    elif r["db_cross_check_note"]:
        print("  db cross-check: %s" % r["db_cross_check_note"])
    print("  sessions scanned: %s   lines skipped (unparseable): %s"
          % (n(r["sessions_scanned"]), n(r["lines_skipped"])))
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
    retention = retention_info(datetime.now(timezone.utc).replace(tzinfo=None))

    if not tdir.is_dir():
        # ABSENT != zero: exit 3, distinct from 0 (measured) and 2 (bad args).
        r = absent_result(project, tdir, since_dt, args.since, until_str,
                          retention)
        if args.json:
            print(json.dumps(r, indent=2))
        else:
            print_human(r)
        return 3

    r = build_result(project, tdir, since_dt, until_dt, args.since, until_str,
                     retention)
    if args.json:
        print(json.dumps(r, indent=2))
    else:
        print_human(r)
    return 0


if __name__ == "__main__":
    sys.exit(main())
