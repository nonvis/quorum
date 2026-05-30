#!/usr/bin/env python3
"""
recap_messages_import.py — deterministic Slack-paste → messages-dump.md formatter.

A Tier-1 helper for the recap knower: turns a curated Slack paste into the
timestamped block format recap parses (`YYYY-MM-DD HH:MM · <source> · <author>`),
with NO LLM in the loop — same input, same output, every run. Dating is
deterministic: it reads Slack's day-divider lines ("Monday, May 25th" /
"May 25th" / "2026-05-25" / "Today"/"Yesterday" with --today) and carries the
current date down to each message; per-message clock times come from the
`Author  [H:MM AM]` stamp Slack emits.

This fixes the copy-paste failure mode: a raw Slack copy DROPS the day dividers,
leaving only clock times across a multi-week thread — undatable. So either
(a) re-select so the dividers come along, or (b) use a Slack JSON export. Either
way YOU curate which messages are relevant (recap is condensed); this tool only
formats + dates what you paste. Messages appearing before the first divider are
skipped with a warning (no date to anchor them) unless --start-date is given.

Read-only on the input; appends to --out (deduped by stamp line) or prints to
stdout. No network, no LLM.

Usage:
  recap_messages_import.py --in paste.txt [--out .quorum/recap/messages-dump.md]
                          [--channel <name>] [--year YYYY] [--today YYYY-MM-DD]
                          [--start-date YYYY-MM-DD] [--selftest]
"""
import argparse
import datetime as dt
import re
import sys
from pathlib import Path

MONTHS = {m: i for i, m in enumerate(
    ["jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct",
     "nov", "dec"], 1)}

DIVIDER_ISO = re.compile(r"^\s*(\d{4})-(\d{2})-(\d{2})\s*$")
DIVIDER_TXT = re.compile(
    r"^\s*(?:(?:mon|tue|wed|thu|fri|sat|sun)[a-z]*\.?,?\s*)?"
    r"(jan|feb|mar|apr|may|jun|jul|aug|sep|oct|nov|dec)[a-z]*\s+"
    r"(\d{1,2})(?:st|nd|rd|th)?(?:,?\s*(\d{4}))?\s*$", re.I)
REL_DIVIDER = re.compile(r"^\s*(today|yesterday)\s*$", re.I)
# "Author  [H:MM AM]" optionally followed by trailing body text on the same line.
STAMP = re.compile(
    r"^(?P<author>.{1,60}?)\s+\[(?P<h>\d{1,2}):(?P<m>\d{2})\s*(?P<ap>[AaPp][Mm])\]"
    r"(?P<rest>.*)$")


def clean_author(a):
    a = re.sub(r"\s*//[^/]*//\s*", " ", a)   # drop "//Greece//"-style tags
    return re.sub(r"\s+", " ", a).strip()


def to_24h(h, m, ap):
    h = int(h) % 12
    if ap.upper() == "PM":
        h += 12
    return f"{h:02d}:{int(m):02d}"


def parse_divider(line, year, today):
    iso = DIVIDER_ISO.match(line)
    if iso:
        return f"{iso.group(1)}-{iso.group(2)}-{iso.group(3)}"
    rel = REL_DIVIDER.match(line)
    if rel:
        if not today:
            return None
        base = dt.date.fromisoformat(today)
        d = base if rel.group(1).lower() == "today" else base - dt.timedelta(days=1)
        return d.isoformat()
    txt = DIVIDER_TXT.match(line)
    if txt:
        mon = MONTHS[txt.group(1).lower()[:3]]
        day = int(txt.group(2))
        yr = int(txt.group(3)) if txt.group(3) else year
        return f"{yr:04d}-{mon:02d}-{day:02d}"
    return None


def parse(text, channel, year, today, start_date):
    """Return (blocks, warnings). Each block is the formatted entry string."""
    source = f"slack#{channel}" if channel else "slack"
    cur_date = start_date
    entries = []        # (date, time24, author, [body lines])
    pending = None
    warnings = []

    def flush():
        nonlocal pending
        if pending is None:
            return
        date, t, author, body = pending
        if date is None:
            warnings.append(f"skipped (no date yet): {author} [{t}]")
        else:
            entries.append((date, t, author, body))
        pending = None

    for raw in text.splitlines():
        line = raw.rstrip()
        d = parse_divider(line, year, today)
        if d is not None:
            cur_date = d
            continue
        m = STAMP.match(line)
        if m:
            flush()
            author = clean_author(m.group("author"))
            t = to_24h(m.group("h"), m.group("m"), m.group("ap"))
            rest = m.group("rest").strip()
            body = [rest] if rest else []
            pending = [cur_date, t, author, body]
        elif pending is not None:
            pending[3].append(line)
    flush()

    blocks = []
    for date, t, author, body in entries:
        lines = [ln.rstrip() for ln in body]
        # collapse leading/trailing blanks; keep internal structure
        while lines and not lines[0].strip():
            lines.pop(0)
        while lines and not lines[-1].strip():
            lines.pop()
        body_txt = "\n".join("  " + ln if ln.strip() else "" for ln in lines)
        block = f"{date} {t} · {source} · {author}"
        if body_txt:
            block += "\n" + body_txt
        blocks.append(block)
    return blocks, warnings


def existing_stamps(path):
    if not path or not Path(path).is_file():
        return set()
    stamps = set()
    for ln in Path(path).read_text(encoding="utf-8").splitlines():
        if re.match(r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2} · ", ln):
            stamps.add(ln.strip())
    return stamps


def selftest():
    fixture = (
        "Monday, May 25th\n"
        "Michael  [2:46 AM]\n"
        "Monday is a US holiday.\n"
        "\n"
        "Sang Soo //Greece//  [10:48 PM]\n"
        "Lean address-based throughout.\n"
        "More detail on the next line.\n"
        "2026-05-28\n"
        "george  [11:39 PM]\n"
        "Created BAS-78/79/80.\n"
    )
    blocks, warns = parse(fixture, "test", 2026, None, None)
    expect = [
        "2026-05-25 02:46 · slack#test · Michael\n  Monday is a US holiday.",
        "2026-05-25 22:48 · slack#test · Sang Soo\n  Lean address-based throughout.\n  More detail on the next line.",
        "2026-05-28 23:39 · slack#test · george\n  Created BAS-78/79/80.",
    ]
    ok = True
    for i, (got, want) in enumerate(zip(blocks, expect)):
        if got != want:
            ok = False
            print(f"FAIL[{i}]:\n--- got ---\n{got}\n--- want ---\n{want}")
        else:
            print(f"PASS[{i}]: {got.splitlines()[0]}")
    if len(blocks) != len(expect):
        ok = False
        print(f"FAIL: block count {len(blocks)} != {len(expect)}")
    # a message before any divider must be skipped (no anchor)
    b2, w2 = parse("alex  [9:00 AM]\nhi\nMay 1st\nbob  [9:01 AM]\nyo\n", "t", 2026, None, None)
    if len(b2) == 1 and w2:
        print("PASS[skip]: pre-divider message skipped with warning")
    else:
        ok = False
        print(f"FAIL[skip]: blocks={len(b2)} warns={w2}")
    print("PASS: recap_messages_import selftest" if ok else "FAIL: selftest")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", default=None,
                    help="input paste file (default: stdin)")
    ap.add_argument("--out", default=None,
                    help="messages-dump.md to append to (default: stdout)")
    ap.add_argument("--channel", default=None,
                    help="slack channel name → source 'slack#<channel>'")
    ap.add_argument("--year", type=int, default=dt.date.today().year,
                    help="year for dividers lacking one (default: this year)")
    ap.add_argument("--today", default=None,
                    help="YYYY-MM-DD to resolve Today/Yesterday dividers")
    ap.add_argument("--start-date", default=None,
                    help="YYYY-MM-DD anchor for messages before the first divider")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    text = (Path(args.inp).read_text(encoding="utf-8") if args.inp
            else sys.stdin.read())
    blocks, warnings = parse(text, args.channel, args.year, args.today,
                             args.start_date)
    for w in warnings:
        print(f"WARNING: {w}", file=sys.stderr)

    seen = existing_stamps(args.out)
    fresh = [b for b in blocks if b.splitlines()[0].strip() not in seen]
    skipped_dup = len(blocks) - len(fresh)

    body = "\n\n".join(fresh)
    if args.out:
        out = Path(args.out)
        prefix = "" if not out.is_file() or out.read_text(encoding="utf-8").endswith("\n\n") else "\n\n"
        with out.open("a", encoding="utf-8") as fh:
            if fresh:
                fh.write(prefix + body + "\n")
        print(f"recap_messages_import → {out}: +{len(fresh)} entries"
              f"{f', {skipped_dup} dup skipped' if skipped_dup else ''}"
              f"{f', {len(warnings)} undated skipped' if warnings else ''}",
              file=sys.stderr)
    else:
        print(body)
    return 0


if __name__ == "__main__":
    sys.exit(main())
