import { useState } from "react";
import type { Agent } from "../types";
import { startConversation } from "../api";
import { MODE, modeOf, type ConversationMode } from "../lib/theme";
import { FlightComposer } from "./FlightComposer";

// Two orthogonal axes, both visible here:
//   ENGINE — run now (daemon dispatches agents immediately) vs queue a flight
//            (autopilot: generate a SUPERVISOR.md plan; a terminal session
//            flies it overnight). Autopilot is a second engine, NOT a mode.
//   MODE   — the write surface (generic writes / brainstorm read-only),
//            applying to either engine.
// The band states the consequence of the current combination in plain
// language so "what am I about to run" is impossible to miss.

type Engine = "daemon" | "autopilot";

const NIGHT = "#8fa9e8";

export function Composer({
  onSubmit,
  busy,
  agents,
}: {
  onSubmit: () => void;
  busy: boolean;
  agents: Agent[];
}) {
  const [goal, setGoal] = useState("");
  const [mode, setMode] = useState<ConversationMode>("generic");
  // Deep link: #flight opens straight into the flight composer.
  const [engine, setEngine] = useState<Engine>(() =>
    location.hash === "#flight" ? "autopilot" : "daemon",
  );
  const [loading, setLoading] = useState(false);

  const disabled = loading || busy;
  const m = modeOf(mode);
  const flight = engine === "autopilot";

  const banner = flight
    ? mode === "generic"
      ? "✈  FLIGHT × GENERIC — OVERNIGHT BUILD · SUBAGENTS MAY MODIFY THE PROJECT"
      : "✈  FLIGHT × BRAINSTORM — OVERNIGHT EXPLORE · READ-ONLY"
    : m.banner;

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!goal.trim() || disabled) return;
    setLoading(true);
    try {
      await startConversation(goal.trim(), mode);
      setGoal("");
      onSubmit();
    } finally {
      setLoading(false);
    }
  };

  const enginePill = (id: Engine, label: string, hint: string) => {
    const active = engine === id;
    const color = id === "autopilot" ? NIGHT : "#63b3a6";
    return (
      <button
        key={id}
        type="button"
        onClick={() => setEngine(id)}
        className="inline-flex items-center gap-2 rounded-full px-3.5 py-1.5 font-mono text-xs transition-colors"
        style={{
          background: active ? `color-mix(in srgb, ${color} 12%, transparent)` : "transparent",
          color: active ? color : "#8a8390",
          border: `1px solid ${active ? `color-mix(in srgb, ${color} 40%, transparent)` : "#2a2632"}`,
        }}
      >
        {label}
        <span className="text-[10.5px]" style={{ color: "#5f5966" }}>
          {hint}
        </span>
      </button>
    );
  };

  const modePill = (id: ConversationMode) => {
    const active = mode === id;
    const ms = MODE[id];
    return (
      <button
        key={id}
        type="button"
        onClick={() => setMode(id)}
        disabled={!flight && disabled}
        className="inline-flex items-center gap-2 rounded-full px-3.5 py-1.5 font-mono text-xs transition-colors disabled:opacity-50 disabled:cursor-not-allowed"
        style={{
          background: active ? ms.chipBg : "transparent",
          color: active ? ms.color : "#8a8390",
          border: `1px solid ${active ? ms.chipBorder : "#2a2632"}`,
        }}
      >
        <span
          className="inline-block h-[7px] w-[7px]"
          style={{ background: ms.color, borderRadius: id === "generic" ? "2px" : "50%" }}
        />
        {ms.label}
        <span className="text-[10.5px]" style={{ color: "#5f5966" }}>
          {ms.hint === "read-only" ? "read-only" : "writes files"}
        </span>
      </button>
    );
  };

  return (
    <form
      onSubmit={handleSubmit}
      className="overflow-hidden rounded-2xl bg-panel transition-colors"
      style={{ border: `1px solid ${flight ? "rgba(143,169,232,0.35)" : m.chipBorder}` }}
    >
      <div
        className="flex items-center px-4 py-2 transition-colors"
        style={{ background: flight ? "rgba(143,169,232,0.07)" : m.bandBg }}
      >
        <span
          className="font-mono text-[10.5px] font-bold tracking-[0.1em]"
          style={{ color: flight ? NIGHT : m.color }}
        >
          {banner}
        </span>
      </div>

      <div className="flex flex-wrap items-center gap-2 px-4 pt-3.5">
        {enginePill("daemon", "▶ run now", "daemon")}
        {enginePill("autopilot", "✈ queue a flight", "overnight")}
        <span className="mx-1 h-5 w-px bg-line-soft" />
        {modePill("generic")}
        {modePill("brainstorm")}
      </div>

      {flight ? (
        <FlightComposer mode={mode} agents={agents} onPlanned={onSubmit} />
      ) : (
        <div className="px-4 pb-4 pt-3">
          <div className="flex gap-2.5">
            <input
              type="text"
              value={goal}
              onChange={(e) => setGoal(e.target.value)}
              placeholder={busy ? "Conversation in progress…" : "What should the quorum work on?"}
              disabled={disabled}
              className="min-w-0 flex-1 rounded-xl border border-line-soft bg-field px-3.5 py-2.5 text-sm text-ink outline-none focus:border-[#55506a] disabled:opacity-50"
            />
            <button
              type="submit"
              disabled={!goal.trim() || disabled}
              className="flex-shrink-0 rounded-xl px-5 py-2.5 text-sm font-bold text-[#171319] transition-opacity disabled:cursor-not-allowed"
              style={{ background: m.color, opacity: !goal.trim() || disabled ? 0.45 : 1 }}
            >
              {loading ? "…" : "Start"}
            </button>
          </div>
        </div>
      )}
    </form>
  );
}
