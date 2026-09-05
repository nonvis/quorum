import { useState, useEffect } from "react";
import type { Conversation, Task } from "../types";
import { fetchConversation, respondToLeader, updateMaxRounds } from "../api";
import { lastHumanGateMessage } from "../lib/segments";
import { deriveVerdict, VERDICT_COLOR } from "../lib/verdict";
import { GateChips } from "./GateActions";
import { ConversationBadges } from "./ConversationBadges";
import {
  modeOf,
  stateOf,
  GLYPH,
  GLYPH_COLOR,
  fmtUsd,
  fmtElapsedSince,
} from "../lib/theme";

export function ConversationCard({
  conversation,
  onOpen,
  onAction,
}: {
  conversation: Conversation;
  onOpen: (id: number, opts?: { respond?: boolean }) => void;
  onAction: () => void;
}) {
  const c = conversation;
  const [tasks, setTasks] = useState<Task[]>([]);
  const [now, setNow] = useState(() => Date.now());

  useEffect(() => {
    fetchConversation(c.id).then((data) => setTasks(data.tasks ?? []));
  }, [c.id, c.state, c.round]);

  // Poll tasks while active so the in-flight agent shows live.
  useEffect(() => {
    if (c.state !== "active") return;
    const id = setInterval(() => {
      fetchConversation(c.id).then((data) => setTasks(data.tasks ?? []));
    }, 2000);
    return () => clearInterval(id);
  }, [c.id, c.state]);

  const activeTask = tasks.find((t) => t.status === "active");
  useEffect(() => {
    if (!activeTask) return;
    setNow(Date.now());
    const id = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(id);
  }, [activeTask?.id]);

  const m = modeOf(c.mode);
  const st = stateOf(c.state);
  const needsYou = c.state === "waiting_for_human";
  const gatePreview = needsYou ? lastHumanGateMessage(tasks) ?? c.paused_reason : null;

  // Latest verdict — one glanceable line for the last settled task, so the
  // board reads without opening the detail. Skipped while the gate box is
  // showing (it already carries the ask).
  const lastSettled = [...tasks].reverse().find((t) => t.status === "done" || t.status === "failed");
  const latestVerdict = !needsYou && lastSettled ? deriveVerdict(lastSettled) : null;

  const resume10 = async () => {
    await updateMaxRounds(c.id, c.max_rounds + 10);
    onAction();
  };

  const [sending, setSending] = useState(false);
  const sendCanned = async (text: string) => {
    if (sending) return;
    setSending(true);
    try {
      await respondToLeader(c.id, text);
      onAction();
    } finally {
      setSending(false);
    }
  };

  return (
    <div
      onClick={() => onOpen(c.id)}
      className="relative cursor-pointer rounded-2xl px-[18px] py-[15px] pl-[26px] transition-transform hover:-translate-y-px"
      style={{
        background: needsYou ? "#241d20" : "#1f1c25",
        border: `1px solid ${needsYou ? "rgba(227,164,92,0.5)" : "#2c2834"}`,
        boxShadow: needsYou
          ? "0 0 0 1px rgba(227,164,92,0.18), 0 12px 32px -16px rgba(227,164,92,0.35)"
          : "0 1px 0 rgba(0,0,0,0.2)",
      }}
    >
      {/* mode accent bar */}
      <span
        className="absolute left-[9px] top-4 bottom-4 w-[3px] rounded-full"
        style={{ background: m.color, opacity: 0.85 }}
      />

      <div className="flex min-w-0 items-center gap-2.5">
        <span className="font-mono text-xs text-faint">#{c.id}</span>
        <span
          className="rounded-full px-2.5 py-[3px] font-mono text-[10.5px] font-bold uppercase tracking-[0.06em]"
          style={{ background: st.bg, color: st.color }}
        >
          {st.label}
        </span>
        <span
          className="inline-flex items-center gap-1.5 rounded-full px-2.5 py-[2px] font-mono text-[10.5px] tracking-[0.04em]"
          style={{ border: `1px solid ${m.chipBorder}`, color: m.color, background: m.chipBg }}
        >
          {m.icon} {m.label}
        </span>
        <ConversationBadges conversation={c} />
        <span className="ml-auto font-mono text-[13px] text-[#d8d2ca]">{fmtUsd(c.spent_usd)}</span>
        <span className="font-mono text-[11px] text-faint">
          round {c.round}/{c.max_rounds}
        </span>
      </div>

      <p className="mt-[9px] text-[16px] font-semibold leading-[1.4] text-ink [text-wrap:pretty]">
        {c.goal}
      </p>

      {latestVerdict && latestVerdict.kind !== "none" && (
        <div className="mt-[7px] flex items-baseline gap-2">
          <span
            className="flex-shrink-0 font-mono text-[10px] font-bold"
            style={{ color: VERDICT_COLOR[latestVerdict.kind] }}
          >
            {latestVerdict.kind === "error" ? "✕" : latestVerdict.kind === "handoff" ? "→" : latestVerdict.kind === "vault" ? "▤" : "▸"}
          </span>
          <span
            className="line-clamp-1 text-[12.5px] leading-[1.5]"
            style={{ color: latestVerdict.kind === "error" ? "#c98b81" : "#9b94a3" }}
            title={latestVerdict.text}
          >
            {latestVerdict.text}
          </span>
        </div>
      )}

      {tasks.length > 0 && (
        <div className="mt-[11px] flex flex-wrap items-center gap-2.5">
          {tasks.map((t, i) => (
            <span key={t.id} className="inline-flex items-center gap-1.5 font-mono text-[11.5px]">
              {i > 0 && <span className="text-[#4a4454]">→</span>}
              <span
                style={{ color: GLYPH_COLOR[t.status] ?? "#5f5966" }}
                className={t.status === "active" ? "q-pulse" : ""}
              >
                {GLYPH[t.status] ?? "○"} {t.agent}
              </span>
              {(t.status === "done" || t.status === "failed") && t.cost != null && (
                <span className="text-[10.5px] text-dim">{fmtUsd(t.cost)}</span>
              )}
            </span>
          ))}
          {activeTask && (
            <span className="q-pulse font-mono text-[11px] text-running">
              {activeTask.agent} working · {fmtElapsedSince(activeTask.started_at ?? activeTask.created_at, now)}
            </span>
          )}
        </div>
      )}

      {needsYou && (
        <div
          className="mt-3 rounded-xl px-3 py-[11px]"
          style={{ background: "rgba(227,164,92,0.08)", border: "1px solid rgba(227,164,92,0.30)" }}
          onClick={(e) => e.stopPropagation()}
        >
          <div className="flex items-center gap-3.5">
            <div className="min-w-0 flex-1">
              <div className="mb-1 font-mono text-[10px] font-bold tracking-[0.12em] text-brand">
                {(c.current_agent ?? "leader").toUpperCase()} ASKS
              </div>
              <div className="line-clamp-2 text-[13px] leading-[1.5] text-[#d8d2ca]">
                {gatePreview}
              </div>
            </div>
            <button
              onClick={() => onOpen(c.id, { respond: true })}
              className="flex-shrink-0 rounded-lg bg-brand px-4 py-2 text-[13px] font-bold text-[#1a1410] hover:bg-brand-bright"
            >
              Respond →
            </button>
          </div>
          <div className="mt-2.5 flex flex-wrap items-center gap-1.5">
            {c.mode === "brainstorm" ? (
              <>
                <button
                  disabled={sending}
                  onClick={() => sendCanned("yes")}
                  className="rounded-full px-3.5 py-1.5 text-[12.5px] font-bold text-[#171319] disabled:opacity-45"
                  style={{ background: "#a793e6" }}
                  title='sends: "yes" — approve all staged vault notes'
                >
                  Yes — save all
                </button>
                <button
                  onClick={() => onOpen(c.id, { respond: true })}
                  className="rounded-full border border-line-soft px-3.5 py-1.5 text-[12.5px] font-semibold text-muted hover:border-line-dash hover:text-ink"
                >
                  Review notes…
                </button>
              </>
            ) : (
              <GateChips onSend={sendCanned} disabled={sending} compact />
            )}
          </div>
        </div>
      )}

      {c.state === "paused" && (
        <div
          className="mt-3 flex items-center justify-between gap-3 rounded-lg border border-dashed px-3 py-2"
          style={{ borderColor: "#3a3444" }}
          onClick={(e) => e.stopPropagation()}
        >
          <span className="text-[12.5px] text-[#b3aa98]">
            ⏸ Paused{c.paused_reason ? ` — ${c.paused_reason}` : ""}
          </span>
          <button
            onClick={resume10}
            className="rounded-lg border border-line-dash bg-transparent px-3 py-1.5 text-xs font-semibold text-[#c9c3bd] hover:border-faint hover:text-ink"
          >
            Resume +10 rounds
          </button>
        </div>
      )}
    </div>
  );
}
