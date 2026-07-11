// Flight detail slide-over — the full morning-review read of one flight:
// task ledger with per-task condensed outcomes, the morning review block,
// and the terminal launch/resume command. Same shell as ConversationDetail.
import { useState } from "react";
import type { Flight } from "../types";
import { setFlightsReviewed } from "../api";
import { stripInlineMd } from "../lib/verdict";
import { modeOf, flightStateOf, fmtWhen, NIGHT } from "../lib/theme";
import { ledgerGlyph } from "./FlightCard";

export function FlightDetail({
  flight,
  onClose,
  onAction,
}: {
  flight: Flight;
  onClose: () => void;
  onAction: () => void;
}) {
  const f = flight;
  const st = flightStateOf(f.status);
  const m = f.mode ? modeOf(f.mode) : null;
  const [busy, setBusy] = useState(false);
  const [copied, setCopied] = useState(false);

  const banner =
    f.status === "needs_you"
      ? "✈  FLIGHT STOPPED — NEEDS YOUR CALL · RESUME IN YOUR TERMINAL"
      : f.status === "in_flight"
        ? "✈  FLIGHT IN PROGRESS — RUNNING OR AWAITING RESUME IN YOUR TERMINAL"
        : f.status === "ready"
          ? "✈  FLIGHT READY — LAUNCH IT FROM YOUR TERMINAL"
          : "✈  FLIGHT LANDED — MORNING REVIEW";

  const outcomesFor = (index: number) =>
    f.outcomes.filter((o) => o.taskIndex === index).flatMap((o) => o.bullets);
  const strayOutcomes = f.outcomes.filter((o) => o.taskIndex === null);

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

  const copyCommand = async () => {
    try {
      await navigator.clipboard.writeText(f.launchCommand);
      setCopied(true);
      setTimeout(() => setCopied(false), 1600);
    } catch {}
  };

  const blocked =
    f.morningReview &&
    f.morningReview.blockedOn &&
    !/^none\.?$/i.test(f.morningReview.blockedOn.trim());

  return (
    <div
      className="q-fade fixed inset-0 z-[60] flex justify-end"
      style={{ background: "rgba(11,9,15,0.55)", backdropFilter: "blur(3px)" }}
      onClick={onClose}
    >
      <div
        onClick={(e) => e.stopPropagation()}
        className="q-slide flex h-full w-[820px] max-w-[96vw] flex-col bg-sheet"
        style={{ borderLeft: "1px solid #322d3c", boxShadow: "-24px 0 64px rgba(0,0,0,0.45)" }}
      >
        {/* engine band */}
        <div
          className="flex flex-shrink-0 items-center gap-2.5 border-b border-line px-5 py-2.5"
          style={{ background: "rgba(143,169,232,0.07)" }}
        >
          <span className="font-mono text-[11px] font-bold tracking-[0.09em]" style={{ color: NIGHT }}>
            {banner}
          </span>
          <span className="flex-1" />
          <button
            onClick={onClose}
            className="h-7 w-7 rounded-lg border border-line-edge text-[13px] text-muted hover:border-line-dash hover:text-ink"
          >
            ✕
          </button>
        </div>

        {/* meta + name */}
        <div className="flex-shrink-0 border-b border-line px-6 pb-4 pt-[18px]">
          <div className="mx-auto flex max-w-[720px] flex-wrap items-center gap-2.5">
            <span
              className="rounded-full px-2.5 py-[3px] font-mono text-[10.5px] font-bold uppercase tracking-[0.06em]"
              style={{ background: st.bg, color: st.color }}
            >
              {st.label}
            </span>
            {m && (
              <span
                className="inline-flex items-center gap-1.5 rounded-full px-2.5 py-[2px] font-mono text-[10.5px]"
                style={{ border: `1px solid ${m.chipBorder}`, color: m.color, background: m.chipBg }}
              >
                {m.icon} {m.label}
              </span>
            )}
            {f.fixture && (
              <span
                className="rounded-full border border-dashed px-2 py-[2px] font-mono text-[10px] tracking-[0.06em]"
                style={{ borderColor: "#4a4454", color: "#8a8390" }}
              >
                FIXTURE — synthetic demo flight
              </span>
            )}
            <span className="flex-1" />
            <span className="font-mono text-[11px] text-faint" title={`created ${f.createdAt ?? "?"}`}>
              {fmtWhen(f.createdAt)} → {fmtWhen(f.updatedAt)}
            </span>
          </div>
          <h2 className="mx-auto mt-2.5 max-w-[720px] text-[19px] font-bold leading-[1.4] text-ink-bright [text-wrap:pretty]">
            {f.name}
          </h2>
        </div>

        <div className="flex-1 overflow-y-auto px-6 pb-6 pt-4">
          <div className="mx-auto flex max-w-[720px] flex-col gap-5">
            {/* blocked-on first — it's why you're here */}
            {blocked && (
              <div
                className="rounded-xl px-3.5 py-3"
                style={{ background: "rgba(227,164,92,0.08)", border: "1px solid rgba(227,164,92,0.30)" }}
              >
                <div className="mb-1 font-mono text-[10px] font-bold tracking-[0.12em] text-brand">
                  BLOCKED ON YOU
                </div>
                <div className="whitespace-pre-wrap text-[13.5px] leading-[1.55] text-[#d8d2ca]">
                  {f.morningReview!.blockedOn}
                </div>
              </div>
            )}

            {/* task ledger + condensed outcomes */}
            <div>
              <div className="mb-2 font-mono text-[10.5px] font-bold tracking-[0.14em] text-faint">
                MAJOR TASKS · {f.tasks.filter((t) => t.status === "done").length}/{f.tasks.length}
              </div>
              <div className="flex flex-col gap-3">
                {f.tasks.map((t) => {
                  const g = ledgerGlyph(t.status, t.warn, f.status);
                  const bullets = outcomesFor(t.index);
                  return (
                    <div key={t.index} className="relative pl-6">
                      <span
                        className={`absolute left-0 top-[2px] font-mono text-[14px] ${t.status === "in_flight" ? "q-pulse" : ""}`}
                        style={{ color: g.color }}
                      >
                        {g.glyph}
                      </span>
                      <div className="text-[13.5px] font-semibold leading-[1.45] text-ink">
                        <span className="mr-1.5 font-mono text-[11px] text-faint">{t.index}.</span>
                        {t.title}
                      </div>
                      {bullets.length > 0 && (
                        <div className="mt-1 flex flex-col gap-1">
                          {bullets.map((b, i) => (
                            <div key={i} className="flex items-baseline gap-2">
                              <span className="flex-shrink-0 font-mono text-[10px]" style={{ color: NIGHT }}>
                                ▸
                              </span>
                              <span className="text-[12.5px] leading-[1.55] text-[#9b94a3]">
                                {stripInlineMd(b)}
                              </span>
                            </div>
                          ))}
                        </div>
                      )}
                    </div>
                  );
                })}
                {f.tasks.length === 0 && (
                  <p className="text-[13px] text-faint">
                    No tasks yet — the supervisor populates the checkpoint from SUPERVISOR.md on first run.
                  </p>
                )}
              </div>
              {strayOutcomes.length > 0 && (
                <div className="mt-3 flex flex-col gap-1">
                  {strayOutcomes.map((o, i) => (
                    <div key={i} className="text-[12.5px] leading-[1.55] text-[#9b94a3]">
                      <span className="font-mono text-[11px] text-faint">{o.heading}: </span>
                      {o.bullets.map(stripInlineMd).join(" · ")}
                    </div>
                  ))}
                </div>
              )}
            </div>

            {/* morning review */}
            {f.morningReview && (
              <div className="rounded-xl border border-line-soft bg-field px-3.5 py-3">
                <div className="mb-2 font-mono text-[10.5px] font-bold tracking-[0.14em]" style={{ color: NIGHT }}>
                  MORNING REVIEW
                </div>
                <dl className="flex flex-col gap-1.5 text-[12.5px] leading-[1.55]">
                  <div className="flex gap-2">
                    <dt className="w-[74px] flex-shrink-0 font-mono text-[11px] text-done">done</dt>
                    <dd className="text-[#b9b2ba]">{f.morningReview.done || "—"}</dd>
                  </div>
                  <div className="flex gap-2">
                    <dt className="w-[74px] flex-shrink-0 font-mono text-[11px] text-paused">pending</dt>
                    <dd className="text-[#b9b2ba]">{f.morningReview.pending || "—"}</dd>
                  </div>
                  <div className="flex gap-2">
                    <dt
                      className="w-[74px] flex-shrink-0 font-mono text-[11px]"
                      style={{ color: blocked ? "#e3a45c" : "#6a6470" }}
                    >
                      blocked-on
                    </dt>
                    <dd style={{ color: blocked ? "#e8c49a" : "#b9b2ba" }}>{f.morningReview.blockedOn || "none"}</dd>
                  </div>
                  {f.morningReview.notes && (
                    <div className="flex gap-2">
                      <dt className="w-[74px] flex-shrink-0 font-mono text-[11px] text-faint">notes</dt>
                      <dd className="text-[#9b94a3]">{f.morningReview.notes}</dd>
                    </div>
                  )}
                </dl>
              </div>
            )}

            {/* terminal command */}
            <div>
              <div className="mb-1.5 font-mono text-[10.5px] font-bold tracking-[0.12em] text-faint">
                {f.status === "complete" ? "FLY THE NEXT PLAN / RE-OPEN" : "LAUNCH · RESUME (terminal)"}
              </div>
              <div className="flex items-center gap-2">
                <input
                  readOnly
                  value={f.launchCommand}
                  onFocus={(e) => e.currentTarget.select()}
                  className="min-w-0 flex-1 rounded-xl border border-line-soft bg-field px-3.5 py-2.5 font-mono text-[12px] text-ink outline-none"
                />
                <button
                  onClick={copyCommand}
                  className="flex-shrink-0 rounded-xl px-4 py-2.5 text-[13px] font-bold text-[#171319]"
                  style={{ background: NIGHT }}
                >
                  {copied ? "Copied ✓" : "Copy"}
                </button>
              </div>
            </div>

            <div className="font-mono text-[10.5px] text-dim">
              source: {f.source} · spec {f.specVersion ?? "?"} · {f.id}
            </div>
          </div>
        </div>

        {/* triage footer */}
        <div className="flex flex-shrink-0 items-center gap-3 border-t border-line-edge px-6 py-3.5">
          <span className="text-[12px] text-faint">
            {f.reviewed ? "cleared from the morning review" : "reviewed it? clear it off the board"}
          </span>
          <span className="flex-1" />
          {!f.reviewed ? (
            <button
              disabled={busy}
              onClick={() => setReviewed(true)}
              className="rounded-lg border px-4 py-2 text-[13px] font-semibold disabled:opacity-45"
              style={{ borderColor: "rgba(133,189,147,0.4)", color: "#85bd93", background: "rgba(133,189,147,0.06)" }}
            >
              ✓ Looks good — clear
            </button>
          ) : (
            <button
              disabled={busy}
              onClick={() => setReviewed(false)}
              className="rounded-lg border border-line-soft px-4 py-2 text-[13px] text-muted hover:text-ink disabled:opacity-45"
            >
              restore to board
            </button>
          )}
        </div>
      </div>
    </div>
  );
}
