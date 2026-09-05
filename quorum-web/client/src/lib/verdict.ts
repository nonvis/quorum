// Verdict derivation — the "less reading" primitive. Every task unit renders
// ONE always-visible line (the verdict); full prose stays collapsed and
// not-expanding is the happy path.
//
// A daemon-emitted `summary` on Task is authoritative: it wins, and it is
// rendered as sent. Everything else is DERIVED here — an explicit
// `VERDICT:`/`Verdict:`/`TL;DR:` line, else the first sentence of the first
// prose paragraph — and only derived text gets the 180-char clip.
import { parseSegments } from "./segments";

export interface Verdict {
  text: string;
  kind: "verdict" | "handoff" | "vault" | "error" | "none";
  // true when taken from an explicit summary (field or VERDICT: line) rather
  // than heuristically from the first sentence
  explicit: boolean;
}

const MAX_LEN = 180;

// Every verdict is one line, whatever its source.
function oneLine(s: string): string {
  return s.trim().replace(/\s+/g, " ");
}

// Clip a CLIENT-DERIVED verdict (a VERDICT: line, a first sentence, a handoff
// preview) — text we cut out of prose ourselves, at an arbitrary point.
//
// A daemon-provided `task.summary` does NOT come through here. The daemon
// already clipped it: 200 bytes, at a word boundary, with its own "…". Passing
// it through a second, shorter, non-word-aware clip would cut mid-word and
// stack an ellipsis on an ellipsis — the daemon's summary is authoritative.
function clip(s: string): string {
  const t = oneLine(s);
  return t.length > MAX_LEN ? t.slice(0, MAX_LEN - 1).trimEnd() + "…" : t;
}

// Strip inline markdown so the verdict reads as plain text on one line.
// Exported for other one-line renderings (flight outcome bullets).
export function stripInlineMd(s: string): string {
  return plain(s);
}

function plain(s: string): string {
  return s
    .replace(/!\[([^\]]*)\]\([^)]*\)/g, "$1")
    .replace(/\[([^\]]+)\]\([^)]*\)/g, "$1")
    .replace(/(\*\*|__)(.*?)\1/g, "$2")
    .replace(/(\*|_)(.*?)\1/g, "$2")
    .replace(/`([^`]*)`/g, "$1")
    .replace(/^#{1,6}\s+/gm, "")
    .replace(/^\s*[-*+]\s+/gm, "")
    .replace(/^\s*\d+\.\s+/gm, "");
}

// First sentence of a plain-text paragraph. Falls back to the whole
// paragraph when no sentence boundary is found.
function firstSentence(s: string): string {
  const m = s.match(/^[\s\S]*?[.!?](?=["')\]]?(\s|$))/);
  return (m ? m[0] : s).trim();
}

const EXPLICIT_RE = /^(?:\*\*)?(?:VERDICT|Verdict|TL;?DR)(?:\*\*)?\s*:\s*(.+)$/m;

export function deriveVerdict(task: {
  result: string | null;
  error: string | null;
  status: string;
  summary?: string | null;
}): Verdict {
  // A daemon-provided summary always wins, and is rendered verbatim — see
  // clip() above for why it is not re-clipped here.
  if (task.summary?.trim()) {
    return { text: oneLine(plain(task.summary)), kind: "verdict", explicit: true };
  }

  if (task.error?.trim()) {
    const line = task.error.trim().split("\n")[0];
    return { text: clip(line), kind: "error", explicit: false };
  }

  if (!task.result?.trim()) {
    return { text: "", kind: "none", explicit: false };
  }

  const explicit = task.result.match(EXPLICIT_RE);
  if (explicit) {
    return { text: clip(plain(explicit[1])), kind: "verdict", explicit: true };
  }

  const segments = parseSegments(task.result);

  for (const seg of segments) {
    if (seg.kind !== "prose") continue;
    // First non-heading, non-empty paragraph → first sentence.
    const paragraphs = seg.text.split(/\n{2,}/);
    for (const p of paragraphs) {
      const body = plain(
        p
          .split("\n")
          .filter((ln) => !/^#{1,6}\s/.test(ln.trim()) && !/^[-=*_]{3,}\s*$/.test(ln.trim()))
          .join(" "),
      ).trim();
      if (body) return { text: clip(firstSentence(body)), kind: "verdict", explicit: false };
    }
  }

  // No prose at all — a pure protocol turn. Summarize the block instead.
  const handoff = segments.find((s) => s.kind === "handoff");
  if (handoff && handoff.kind === "handoff") {
    const preview = clip(plain(handoff.prompt.split("\n")[0] ?? ""));
    return {
      text: `→ ${handoff.to}${preview ? ": " + preview : ""}`,
      kind: "handoff",
      explicit: false,
    };
  }
  const vault = segments.find((s) => s.kind === "vault");
  if (vault && vault.kind === "vault") {
    return { text: `staged write → ${vault.path}`, kind: "vault", explicit: false };
  }

  return { text: "", kind: "none", explicit: false };
}

// Verdict accent color per kind (verdict text itself stays ink — the glyph
// carries the kind).
export const VERDICT_COLOR: Record<Verdict["kind"], string> = {
  verdict: "#9b94a3",
  handoff: "#8fa9e8",
  vault: "#a793e6",
  error: "#c98b81",
  none: "#5f5966",
};
