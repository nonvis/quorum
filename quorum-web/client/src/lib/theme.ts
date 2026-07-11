// Runtime-chosen colors for the quorum dashboard. Static chrome colors live in
// index.css (@theme); these are picked from data (a conversation's mode, a
// task's status, an agent's role) so they're applied as inline styles.
//
// Mirrors the color system from the Quorum Redesign mock. Keep the hex values
// in sync with the --color-* tokens in index.css.

export type ConversationMode = "generic" | "brainstorm";

export interface ModeStyle {
  label: string;
  icon: string;
  color: string;
  banner: string;
  bandBg: string;
  chipBg: string;
  chipBorder: string;
  hint: string;
}

export const MODE: Record<string, ModeStyle> = {
  generic: {
    label: "generic",
    icon: "⬢",
    color: "#63b3a6",
    banner: "⬢  GENERIC — AGENTS MAY MODIFY THE PROJECT",
    bandBg: "rgba(99,179,166,0.08)",
    chipBg: "rgba(99,179,166,0.10)",
    chipBorder: "rgba(99,179,166,0.35)",
    hint: "agents can write",
  },
  brainstorm: {
    label: "brainstorm",
    icon: "◈",
    color: "#a793e6",
    banner: "◈  BRAINSTORM — READ-ONLY · WRITES ARE STAGED FOR YOUR APPROVAL",
    bandBg: "rgba(167,147,230,0.08)",
    chipBg: "rgba(167,147,230,0.10)",
    chipBorder: "rgba(167,147,230,0.35)",
    hint: "read-only",
  },
};

export function modeOf(mode: string | null | undefined): ModeStyle {
  return MODE[mode ?? "generic"] ?? MODE.generic;
}

export interface StateStyle {
  label: string;
  color: string;
  bg: string;
}

// Display relabels: active → "running", waiting_for_human → "needs you".
export const STATE: Record<string, StateStyle> = {
  active: { label: "running", color: "#8fa9e8", bg: "rgba(143,169,232,0.12)" },
  waiting_for_human: { label: "needs you", color: "#191310", bg: "#e3a45c" },
  done: { label: "done", color: "#85bd93", bg: "rgba(133,189,147,0.12)" },
  paused: { label: "paused", color: "#b3aa98", bg: "rgba(179,170,152,0.12)" },
  closed: { label: "closed", color: "#c98b81", bg: "rgba(201,139,129,0.12)" },
};

export function stateOf(state: string): StateStyle {
  return STATE[state] ?? { label: state.replace(/_/g, " "), color: "#8a8390", bg: "rgba(138,131,144,0.12)" };
}

export const ROLE: Record<string, string> = {
  leader: "#c79be8",
  thinker: "#8fa9e8",
  doer: "#85bd93",
  evaluator: "#e08ba8",
};

export const ROLE_INITIAL: Record<string, string> = {
  leader: "L",
  thinker: "T",
  doer: "D",
  evaluator: "E",
};

export function roleColor(role: string): string {
  return ROLE[role] ?? "#8a8390";
}

// Task-status glyph + color for the inline timeline.
export const GLYPH: Record<string, string> = {
  pending: "○",
  active: "◉",
  done: "✓",
  failed: "✕",
};
export const GLYPH_COLOR: Record<string, string> = {
  pending: "#5f5966",
  active: "#8fa9e8",
  done: "#85bd93",
  failed: "#c98b81",
};

// ── formatting helpers ────────────────────────────────────────────────
export const fmtUsd = (n: number): string => "$" + (n ?? 0).toFixed(2);

export const ktok = (n: number | null | undefined): string =>
  n == null ? "" : n >= 1000 ? (n / 1000).toFixed(1) + "k" : String(n);

export function fmtElapsedSec(sec: number): string {
  const x = Math.max(0, Math.floor(sec));
  return Math.floor(x / 60) + ":" + String(x % 60).padStart(2, "0");
}

// Elapsed mm:ss since a SQLite UTC timestamp ("YYYY-MM-DD HH:MM:SS").
export function fmtElapsedSince(startedAt: string | null | undefined, nowMs: number): string {
  if (!startedAt) return "";
  const start = Date.parse(startedAt.replace(" ", "T") + "Z");
  if (isNaN(start)) return "";
  return fmtElapsedSec((nowMs - start) / 1000);
}
