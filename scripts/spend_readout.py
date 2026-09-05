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
sum (adding it would double-count the same converse spend). It runs on BOTH
paths: when the transcript dir is absent the ledger is the only reading left for
the window, and it is printed as exactly that — still not the TOTAL.

Munge rule (<abs project path> -> transcript dir): every '/' and '.' in the
absolute project path is replaced with '-', then prefixed by
`<HOME>/.claude/projects/`.
  /Users/sangsoo/nonvis/crucible -> ~/.claude/projects/-Users-sangsoo-nonvis-crucible

Transcript record shape (verified against a real file): each assistant record is
a JSON object per line with a top-level `"timestamp"` (ISO-8601, e.g.
"2026-07-21T06:08:01.953Z"), `"type":"assistant"`, and a `"message"` object
carrying `"id"`, `"model"`, and `"usage":{input_tokens, cache_creation_input_
tokens, cache_read_input_tokens, output_tokens, ...}`. Newer records also carry
`usage.cache_creation:{ephemeral_5m_input_tokens, ephemeral_1h_input_tokens}` —
the TTL split, which we prefer when present because the two tiers bill at
different multiples. Claude Code repeats the SAME message record across several
lines (identical usage), so we dedupe by `message.id` (keep last occurrence) and
count each message ONCE.

$ estimate CAVEAT: subscription usage is not billed per token — the "EST." $
column exists ONLY to compare accumulated tokens against window_budget_usd. Rates
are EXACT-ID list prices (MODEL_RATES, longest-prefix match) with a flagged
per-family fallback (FAMILY_RATES) for ids the table has never seen; a model that
matches neither is priced `n/a`, NEVER $0 — its tokens are still reported and the
TOTAL is then labelled a FLOOR (absent rate != zero spend).

ABSENT != ZERO. Three distinct outcomes, three exit codes:
  * transcript dir ABSENT  -> every $ figure prints `n/a`, exit 3, JSON
    "status":"no_transcript_dir". We cannot see this source; we do not claim it
    spent nothing. The dir is absent either because no Claude Code session ever
    ran with this cwd, or because Claude Code's transcript retention pruned it.
    The daemon ledger cross-check STILL runs and prints, labelled the only
    reading for the window (or `unavailable (<reason>)`); the TOTAL stays n/a.
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

# ── Per-MTok list prices ───────────────────────────────────────────────────
# Re-verified 2026-09-04 against a LOCAL source (this script never fetches the
# web): claude-api skill (Claude Code 2.1.261 bundle), pricing table cached
# 2026-06-24.
#
# EXACT ids, resolved by LONGEST PREFIX: "claude-fable-5-1[1m]" and dated
# suffixes ("claude-opus-5-20260514") resolve to their base row, and
# "claude-fable-5-1" beats "claude-fable-5" because it is longer. The previous
# table keyed by family SUBSTRING, which mispriced Sonnet 5 ($2/$10) at the
# Sonnet 4.6 rate ($3/$15) — hence exact ids.
MODEL_RATES = {
    "claude-fable-5-1":  {"in": 10.0, "out": 50.0},
    "claude-fable-5":    {"in": 10.0, "out": 50.0},
    "claude-opus-5":     {"in": 5.0,  "out": 25.0},
    "claude-opus-4-8":   {"in": 5.0,  "out": 25.0},
    "claude-opus-4-7":   {"in": 5.0,  "out": 25.0},
    "claude-opus-4-6":   {"in": 5.0,  "out": 25.0},
    "claude-sonnet-5":   {"in": 2.0,  "out": 10.0},
    "claude-sonnet-4-6": {"in": 3.0,  "out": 15.0},
    "claude-haiku-4-5":  {"in": 1.0,  "out": 5.0},
}

# Fallback for an id with NO row above (a model released after the cached
# table). Matched by substring, priced at the family's most expensive current
# list rate — a fallback that UNDERstates would quietly shrink the readout — and
# always FLAGGED `family-rate` in the output, so a guess announces itself.
# No family match either => n/a, never $0 (see est_usd).
FAMILY_RATES = {
    "fable":  {"in": 10.0, "out": 50.0},
    "opus":   {"in": 5.0,  "out": 25.0},
    "sonnet": {"in": 3.0,  "out": 15.0},
    "haiku":  {"in": 1.0,  "out": 5.0},
}

# Cache creation is billed per TTL tier: 1.25x input for the 5-minute tier, 2x
# input for the 1-hour tier. Transcripts carry the split under
# usage.cache_creation.ephemeral_{5m,1h}_input_tokens; older records only carry
# the lump `cache_creation_input_tokens`, which we price at the 5m rate AND say
# so in the output (a 1h write would be 1.6x that).
CACHE_CREATE_5M_MULT = 1.25
CACHE_CREATE_1H_MULT = 2.00

# Cache read is 0.1x input — EXCEPT these ids, which bill a FLAT per-MTok rate
# regardless of their input price.
CACHE_READ_MULT = 0.10
FLAT_CACHE_READ_USD = {
    "claude-fable-5-1": 0.25,
    # Listed by the same source; no in/out row here, so it prices n/a today —
    # the flat read rate is recorded so a future row inherits it.
    "claude-mythos-5-1": 0.25,
}

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
    for fam in FAMILY_RATES:
        if fam in m:
            return fam
    return "unknown"


def resolve_rate(model: str):
    """(rates, source, key) — how this model gets priced.

    source: "exact"  an id in MODEL_RATES, matched by LONGEST prefix, so
                     "claude-fable-5-1[1m]" and dated suffixes still resolve;
            "family" no row, but the family substring is known — the caller
                     FLAGS this, because it is a guess about an unseen model;
            "unknown" neither => rates None => est n/a, never $0.
    """
    m = (model or "").lower()
    best = None
    for mid in MODEL_RATES:
        if m.startswith(mid) and (best is None or len(mid) > len(best)):
            best = mid
    if best is not None:
        return MODEL_RATES[best], "exact", best
    fam = rate_family(model)
    if fam != "unknown":
        return FAMILY_RATES[fam], "family", fam
    return None, "unknown", None


def cache_read_rate(model: str, in_rate: float) -> float:
    """Per-MTok cache-read price: 0.1x input, or a model's FLAT override."""
    m = (model or "").lower()
    for mid, flat in FLAT_CACHE_READ_USD.items():
        if m.startswith(mid):
            return flat
    return in_rate * CACHE_READ_MULT


def est_usd(model: str, in_tok: int, out_tok: int, cw5_tok: int, cw1h_tok: int,
            cw_untiered_tok: int, cr_tok: int):
    """Per-MTok estimate, or None when the model matches no rate at all.

    None, NOT 0.0 — a rate we do not have is not a spend of zero. The caller
    reports such a model with `n/a` and marks the TOTAL a FLOOR.

    cw_untiered_tok is cache creation from a record with no TTL split; it is
    priced at the 5m tier and the caller says so in the output.
    """
    r, source, _ = resolve_rate(model)
    if source == "unknown":
        return None
    dollars = (
        in_tok * r["in"]
        + out_tok * r["out"]
        + (cw5_tok + cw_untiered_tok) * r["in"] * CACHE_CREATE_5M_MULT
        + cw1h_tok * r["in"] * CACHE_CREATE_1H_MULT
        + cr_tok * cache_read_rate(model, r["in"])
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
    """(usd, conversations, reason) — conversations.spent_usd inside the window.

    A SECOND, INDEPENDENT reading of the same window: the daemon records what it
    believes it spent, the transcripts record what Claude Code wrote down. On the
    transcript-present path this is a cross-check ONLY — NEVER added to the
    transcript EST (that would double-count the same converse spend, Decision
    #64). On the transcript-ABSENT path it is the ONLY reading we have, so we run
    it there too and label it as such; it still does not become the TOTAL.

    READ-ONLY (mode=ro) open. Any error (locked/missing db, schema mismatch) ->
    (None, None, reason) so a concurrent refresh can't crash us.

    Schema (quorum-core/src/storage/schema.h): conversations(spent_usd REAL,
    created_at TEXT DEFAULT datetime('now')). created_at is UTC "YYYY-MM-DD
    HH:MM:SS", so a fixed-width string compare against the formatted window
    bounds is chronological. (tasks.cost is the per-task figure behind the same
    numbers; conversations is the level the daemon actually settles, so it is
    what we read — summing both would double-count.)
    """
    db_path = Path(project) / ".quorum" / "quorum.db"
    if not db_path.exists():
        return None, None, "no .quorum/quorum.db"
    since_s = since_dt.strftime("%Y-%m-%d %H:%M:%S")
    until_s = until_dt.strftime("%Y-%m-%d %H:%M:%S")
    con = None
    try:
        uri = db_path.resolve().as_uri() + "?mode=ro"
        con = sqlite3.connect(uri, uri=True, timeout=1.0)
        cur = con.execute(
            "SELECT COALESCE(SUM(spent_usd), 0), COUNT(*) FROM conversations "
            "WHERE created_at >= ? AND created_at <= ?",
            (since_s, until_s),
        )
        row = cur.fetchone()
        return float(row[0] or 0.0), int(row[1] or 0), None
    except Exception as e:
        return None, None, "%s reading .quorum/quorum.db" % type(e).__name__
    finally:
        if con is not None:
            try:
                con.close()
            except Exception:
                pass


def split_cache_creation(usage: dict):
    """(5m, 1h, untiered) cache-creation tokens from one `usage` object.

    Prefer usage.cache_creation.ephemeral_{5m,1h}_input_tokens — the TTL split,
    which bills at different multiples. When that sub-object is absent (older
    records), the lump `cache_creation_input_tokens` goes to the UNTIERED bucket
    rather than being assumed 5m silently: the caller prices it at 5m and prints
    that it did. Never sum both — the lump is the total of the split.
    """
    cc = usage.get("cache_creation")
    if isinstance(cc, dict):
        five = int(cc.get("ephemeral_5m_input_tokens") or 0)
        hour = int(cc.get("ephemeral_1h_input_tokens") or 0)
        if five or hour:
            return five, hour, 0
    return 0, 0, int(usage.get("cache_creation_input_tokens") or 0)


def scan_transcripts(tdir: Path, since_dt: datetime, until_dt: datetime):
    """Sum usage per model over records with timestamp in [since, until].

    Pre-filter files by mtime >= (since - 1h slack); parse line-by-line; dedupe
    by message.id (keep last occurrence). Returns (per_model, sessions_scanned,
    lines_skipped, records_counted).
    """
    slack_epoch = calendar.timegm(since_dt.timetuple()) - 3600
    # message.id -> (model, in, out, cw5, cw1h, cw_untiered, cr) — last wins.
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
                cw5, cw1h, cw_untiered = split_cache_creation(usage)
                by_msg[key] = (
                    msg.get("model", "unknown"),
                    int(usage.get("input_tokens") or 0),
                    int(usage.get("output_tokens") or 0),
                    cw5, cw1h, cw_untiered,
                    int(usage.get("cache_read_input_tokens") or 0),
                )

    per_model = {}
    for model, i, o, cw5, cw1h, cwu, cr in by_msg.values():
        m = per_model.setdefault(
            model,
            {"input_tokens": 0, "output_tokens": 0,
             "cache_creation_input_tokens": 0,
             "cache_creation_5m_tokens": 0, "cache_creation_1h_tokens": 0,
             "cache_creation_untiered_tokens": 0,
             "cache_read_input_tokens": 0},
        )
        m["input_tokens"] += i
        m["output_tokens"] += o
        m["cache_creation_5m_tokens"] += cw5
        m["cache_creation_1h_tokens"] += cw1h
        m["cache_creation_untiered_tokens"] += cwu
        # The reported cache-w column stays the TOTAL, split or not.
        m["cache_creation_input_tokens"] += cw5 + cw1h + cwu
        m["cache_read_input_tokens"] += cr

    return per_model, sessions_scanned, lines_skipped, len(by_msg)


def build_result(project, tdir, since_dt, until_dt, since_str, until_str,
                 retention):
    per_model, sessions, skipped, counted = scan_transcripts(tdir, since_dt, until_dt)

    models = []
    unknown_models = []
    family_rate_models = []
    untiered_models = []
    tot = {"input_tokens": 0, "output_tokens": 0,
           "cache_creation_input_tokens": 0, "cache_read_input_tokens": 0,
           "est_usd": 0.0}
    for model in sorted(per_model):
        m = per_model[model]
        e = est_usd(model, m["input_tokens"], m["output_tokens"],
                    m["cache_creation_5m_tokens"], m["cache_creation_1h_tokens"],
                    m["cache_creation_untiered_tokens"],
                    m["cache_read_input_tokens"])
        _, source, key = resolve_rate(model)
        fam = rate_family(model)
        if source == "unknown":
            unknown_models.append(model)
        elif source == "family":
            family_rate_models.append(model)
        if m["cache_creation_untiered_tokens"]:
            untiered_models.append(model)
        models.append({
            "model": model,
            "family": fam,
            # "exact" | "family" | "unknown" — a family rate is a GUESS about a
            # model the priced table has never seen, so it is named in the output.
            "rate_source": source,
            "rate_key": key,
            "input_tokens": m["input_tokens"],
            "output_tokens": m["output_tokens"],
            "cache_creation_input_tokens": m["cache_creation_input_tokens"],
            "cache_creation_5m_tokens": m["cache_creation_5m_tokens"],
            "cache_creation_1h_tokens": m["cache_creation_1h_tokens"],
            "cache_creation_untiered_tokens": m["cache_creation_untiered_tokens"],
            "cache_read_input_tokens": m["cache_read_input_tokens"],
            # None (JSON null) for an unrated model — never 0.0.
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

    db_usd, db_convs, db_reason = db_cross_check(project, since_dt, until_dt)
    db_note = None
    if db_usd is None:
        db_note = ("(db cross-check unavailable: %s)" % db_reason
                   if db_reason else "(db cross-check unavailable)")

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
        "family_rate_models": family_rate_models,
        "cache_creation_untiered_models": untiered_models,
        "total": tot,
        "window_budget_usd": budget,
        "budget_pct_est": budget_pct,
        "db_cross_check_usd": (round(db_usd, 4) if db_usd is not None else None),
        "db_cross_check_conversations": db_convs,
        "db_cross_check_note": db_note,
        "db_cross_check_is_only_reading": False,
        "sessions_scanned": sessions,
        "lines_skipped": skipped,
        "records_counted": counted,
        "since_predates_retention": since_predates,
        "est_caveat": EST_CAVEAT,
    }
    r.update(retention)
    return r


def absent_result(project, tdir, since_dt, until_dt, since_str, until_str,
                  retention):
    """The transcript dir does not exist — every TRANSCRIPT figure is UNKNOWN.

    No token counts, no est_usd, no session count: reporting 0 for any of them
    would be a measurement we did not make. `status` and the exit code carry it.

    But the daemon's own ledger is a SEPARATE source, and it is still readable:
    we run the db cross-check here too and print it labelled as the only reading
    for this window. It is NOT promoted into the TOTAL — the TOTAL stays n/a,
    because the daemon only sees `converse` spend, not the interactive session or
    its subagents (Decision #64). One readable source does not make the missing
    one zero.
    """
    since_predates = since_dt.date().isoformat() < retention["retention_horizon"]
    db_usd, db_convs, db_reason = db_cross_check(project, since_dt, until_dt)
    db_note = None
    if db_usd is None:
        db_note = "unavailable (%s)" % (db_reason or "unknown error")
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
        "family_rate_models": [],
        "cache_creation_untiered_models": [],
        "total": {"input_tokens": None, "output_tokens": None,
                  "cache_creation_input_tokens": None,
                  "cache_read_input_tokens": None,
                  "est_usd": None, "est_is_floor": None},
        "window_budget_usd": read_window_budget(project),
        "budget_pct_est": None,
        "db_cross_check_usd": (round(db_usd, 4) if db_usd is not None else None),
        "db_cross_check_conversations": db_convs,
        "db_cross_check_note": db_note,
        "db_cross_check_is_only_reading": True,
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
    for m in r["models"]:
        if m.get("rate_source") == "family":
            fr = FAMILY_RATES[m["rate_key"]]
            print("  family-rate: %s has no row in the priced table — estimated "
                  "at the '%s' family rate ($%g/$%g per MTok). Verify before "
                  "quoting." % (m["model"], m["rate_key"], fr["in"], fr["out"]))
    if r.get("cache_creation_untiered_models"):
        print("  cache-creation TTL split absent for %s — priced at the "
              "5-minute rate (%gx input); a 1-hour write bills %gx."
              % (", ".join(r["cache_creation_untiered_models"]),
                 CACHE_CREATE_5M_MULT, CACHE_CREATE_1H_MULT))
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
    if r["db_cross_check_usd"] is not None and absent:
        # The transcripts are gone; the daemon's ledger is not. Say exactly what
        # this number is — a real reading of a DIFFERENT source, and the only one
        # for this window — and that the TOTAL above still stands at n/a.
        print("  db cross-check (daemon-recorded, the only reading for this "
              "window): $%.2f across %d conversations"
              % (r["db_cross_check_usd"], r["db_cross_check_conversations"] or 0))
        print("    [conversations.spent_usd — `converse` spend ONLY, so it is a "
              "partial view and is NOT summed into the TOTAL above]")
    elif r["db_cross_check_usd"] is not None:
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
        r = absent_result(project, tdir, since_dt, until_dt, args.since,
                          until_str, retention)
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
