// A flight's morning-review digest card. Optimized for triage-and-clear:
// glance (ledger + verdict bullets) → verify (open detail if needed) →
// dismiss ("Looks good — clear") → never reopen. Needs-you flights carry the
// blocked-on question inline; in-flight ones the honest "resume in your
// terminal" state.
import { useState } from "react";
import type { Flight } from "../types";
import { setFlightsReviewed } from "../api";
import { stripInlineMd } from "../lib/verdict";
import { modeOf, flightStateOf, fmtWhen, NIGHT } from "../lib/theme";
import { FlightLaunch } from "./FlightLaunch";

export function ledgerGlyph(status: string, warn: boolean, flightStatus: string): { glyph: string; color: string } {
  if (status === "done") return warn ? { glyph: "⚠", color: "#e3a45c" } : { glyph: "✓", color: "#85bd93" };
  if (status === "in_flight") return { glyph: "◉", color: "#8fa9e8" };
  // pending: a stopped flight never reached it (✕); otherwise just not yet (○)
  return flightStatus === "needs_you" ? { glyph: "✕", color: "#c98b81" } : { glyph: "○", color: "#5f5966" };
}

export function FlightCard({
  flight,
  onOpen,
  onAction,
}: {
  flight: Flight;
  onOpen: (id: string) => void;
  onAction: () => void;
}) {
  const f = flight;
  const st = flightStateOf(f.status);
  const m = f.mode ? modeOf(f.mode) : null;
  const needsYou = f.status === "needs_you";
  const [busy, setBusy] = useState(false);
  const [copied, setCopied] = useState(false);

  // 2–3 one-line verdict bullets: first bullet of each task outcome, in order.
  const bullets = f.outcomes
    .map((o) => o.bullets[0])
    .filter(Boolean)
    .slice(0, 3)
    .map((b) => stripInlineMd(b));

  const setReviewed = async (reviewed: boolean) => {
    if (busy) return;
    setBusy(true);
    try {
      await setFlightsReviewed([f.id], reviewed);
      onAction();
    } finally {
      setBusy(false);
    }
  };

  const copyCommand = async (e: React.MouseEvent) => {
    e.stopPropagation();
    try {
      await navigator.clipboard.writeText(f.launchCommand);
      setCopied(true);
      setTimeout(() => setCopied(false), 1600);
    } catch {}
  };

  return (
    <div
      onClick={() => onOpen(f.id)}
      className="relative cursor-pointer rounded-2xl px-[18px] py-[15px] pl-[26px] transition-transform hover:-translate-y-px"
      style={{
        background: needsYou ? "#241d20" : "#1f1c25",
        border: `1px solid ${needsYou ? "rgba(227,164,92,0.5)" : "rgba(143,169,232,0.22)"}`,
        boxShadow: needsYou
          ? "0 0 0 1px rgba(227,164,92,0.18), 0 12px 32px -16px rgba(227,164,92,0.35)"
          : "0 1px 0 rgba(0,0,0,0.2)",
        opacity: f.reviewed ? 0.55 : 1,
      }}
    >
      {/* engine accent bar — night blue, not a mode color */}
      <span
        className="absolute left-[9px] top-4 bottom-4 w-[3px] rounded-full"
        style={{ background: NIGHT, opacity: 0.85 }}
      />

      <div className="flex min-w-0 flex-wrap items-center gap-2.5">
        <span
          className="inline-flex items-center gap-1.5 rounded-full px-2.5 py-[2px] font-mono text-[10.5px] font-bold tracking-[0.04em]"
          style={{ border: `1px solid rgba(143,169,232,0.4)`, color: NIGHT, background: "rgba(143,169,232,0.1)" }}
          title="autopilot engine — an overnight terminal flight, not a live conversation"
        >
          ✈ flight
        </span>
        <span
          className="rounded-full px-2.5 py-[3px] font-mono text-[10.5px] font-bold uppercase tracking-[0.06em]"
          style={{ background: st.bg, color: st.color }}
        >
          {st.label}
        </span>
        {m && (
          <span
            className="inline-flex items-center gap-1.5 rounded-full px-2.5 py-[2px] font-mono text-[10.5px] tracking-[0.04em]"
            style={{ border: `1px solid ${m.chipBorder}`, color: m.color, background: m.chipBg }}
          >
            {m.icon} {m.label}
          </span>
        )}
        {f.fixture && (
          <span
            className="rounded-full border border-dashed px-2 py-[2px] font-mono text-[10px] tracking-[0.06em]"
            style={{ borderColor: "#4a4454", color: "#8a8390" }}
            title="synthetic demo flight — safe to delete"
          >
            FIXTURE
          </span>
        )}
        {f.reviewed && (
          <span className="font-mono text-[10.5px] text-faint">cleared ✓</span>
        )}
        <span className="ml-auto font-mono text-[11px] text-faint" title={f.updatedAt ?? ""}>
          {fmtWhen(f.updatedAt)}
        </span>
      </div>

      <p className="mt-[9px] text-[16px] font-semibold leading-[1.4] text-ink [text-wrap:pretty]">
        {f.name}
      </p>

      {/* outcome ledger */}
      {f.tasks.length > 0 && (
        <div className="mt-[10px] flex flex-wrap items-center gap-2">
          {f.tasks.map((t) => {
            const g = ledgerGlyph(t.status, t.warn, f.status);
            return (
              <span
                key={t.index}
                className={`font-mono text-[13px] ${t.status === "in_flight" ? "q-pulse" : ""}`}
                style={{ color: g.color }}
                title={`Task ${t.index}: ${t.title}`}
              >
                {g.glyph}
              </span>
            );
          })}
          <span className="font-mono text-[11px] text-faint">
            {f.tasks.filter((t) => t.status === "done").length}/{f.tasks.length} tasks
          </span>
        </div>
      )}

      {/* verdict bullets */}
      {bullets.length > 0 && (
        <div className="mt-2 flex flex-col gap-1">
          {bullets.map((b, i) => (
            <div key={i} className="flex items-baseline gap-2">
              <span className="flex-shrink-0 font-mono text-[10px]" style={{ color: NIGHT }}>
                ▸
              </span>
              <span className="line-clamp-1 text-[12.5px] leading-[1.5] text-[#9b94a3]" title={b}>
                {b}
              </span>
            </div>
          ))}
        </div>
      )}

      {/* needs-you: the blocked-on question, pulled up */}
      {needsYou && f.morningReview?.blockedOn && (
        <div
          className="mt-3 rounded-xl px-3 py-[11px]"
          style={{ background: "rgba(227,164,92,0.08)", border: "1px solid rgba(227,164,92,0.30)" }}
          onClick={(e) => e.stopPropagation()}
        >
          <div className="mb-1 font-mono text-[10px] font-bold tracking-[0.12em] text-brand">
            FLIGHT STOPPED — NEEDS YOUR CALL
          </div>
          <div className="line-clamp-3 text-[13px] leading-[1.5] text-[#d8d2ca]">
            {f.morningReview.blockedOn}
          </div>
          <div className="mt-2 flex items-center gap-2">
            <span className="text-[11.5px] text-faint">answer it where the flight runs:</span>
            <code className="rounded bg-chip px-2 py-1 font-mono text-[11px] text-ink">{f.launchCommand}</code>
            <button
              onClick={copyCommand}
              className="rounded-md border border-line-soft px-2 py-1 font-mono text-[10.5px] text-muted hover:text-ink"
            >
              {copied ? "copied ✓" : "copy"}
            </button>
          </div>
        </div>
      )}

      {/* in-flight: honest terminal-resume state */}
      {f.status === "in_flight" && (
        <div
          className="mt-3 flex flex-wrap items-center gap-2 rounded-xl px-3 py-2.5"
          style={{ background: "rgba(143,169,232,0.07)", border: "1px solid rgba(143,169,232,0.3)" }}
          onClick={(e) => e.stopPropagation()}
        >
          <span className="q-pulse font-mono text-[12px]" style={{ color: NIGHT }}>
            ◉ flight in progress
          </span>
          <span className="text-[12px] text-muted">
            running or awaiting resume — it lives in your terminal:
          </span>
          <code className="rounded bg-chip px-2 py-1 font-mono text-[11px] text-ink">{f.launchCommand}</code>
          <button
            onClick={copyCommand}
            className="rounded-md border border-line-soft px-2 py-1 font-mono text-[10.5px] text-muted hover:text-ink"
          >
            {copied ? "copied ✓" : "copy"}
          </button>
        </div>
      )}

      {/* triage actions */}
      <div className="mt-3 flex items-center gap-2" onClick={(e) => e.stopPropagation()}>
        {f.source === "checkpoint" && f.status !== "complete" && (
          <FlightLaunch onLaunched={onAction} />
        )}
        {!f.reviewed ? (
          <button
            disabled={busy}
            onClick={() => setReviewed(true)}
            className="rounded-lg border px-3.5 py-1.5 text-[12.5px] font-semibold transition-colors disabled:opacity-45"
            style={{ borderColor: "rgba(133,189,147,0.4)", color: "#85bd93", background: "rgba(133,189,147,0.06)" }}
          >
            ✓ Looks good — clear
          </button>
        ) : (
          <button
            disabled={busy}
            onClick={() => setReviewed(false)}
            className="rounded-lg border border-line-soft px-3.5 py-1.5 text-[12.5px] text-muted hover:text-ink disabled:opacity-45"
          >
            restore
          </button>
        )}
        <span className="ml-auto font-mono text-[10.5px] text-dim">{f.id === "current" ? "live checkpoint" : f.id}</span>
      </div>
    </div>
  );
}
