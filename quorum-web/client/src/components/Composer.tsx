import { useState } from "react";
import { startConversation } from "../api";
import { MODE, modeOf, type ConversationMode } from "../lib/theme";

// The composer's mode is the mode a NEW conversation starts in. The band states
// the consequence in plain language so "what mode am I about to run in" is
// impossible to miss, and the Start button + border take the mode's color.
export function Composer({ onSubmit, busy }: { onSubmit: () => void; busy: boolean }) {
  const [goal, setGoal] = useState("");
  const [mode, setMode] = useState<ConversationMode>("generic");
  const [loading, setLoading] = useState(false);

  const disabled = loading || busy;
  const m = modeOf(mode);

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

  const pill = (id: ConversationMode) => {
    const active = mode === id;
    const ms = MODE[id];
    return (
      <button
        key={id}
        type="button"
        onClick={() => setMode(id)}
        disabled={disabled}
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
        <span className="text-[10.5px]" style={{ color: "#5f5966" }}>{ms.hint === "read-only" ? "read-only" : "writes files"}</span>
      </button>
    );
  };

  return (
    <form
      onSubmit={handleSubmit}
      className="overflow-hidden rounded-2xl bg-panel transition-colors"
      style={{ border: `1px solid ${m.chipBorder}` }}
    >
      <div
        className="flex items-center px-4 py-2 transition-colors"
        style={{ background: m.bandBg }}
      >
        <span
          className="font-mono text-[10.5px] font-bold tracking-[0.1em]"
          style={{ color: m.color }}
        >
          {m.banner}
        </span>
      </div>
      <div className="px-4 pb-4 pt-3.5">
        <div className="mb-3 flex gap-2">
          {pill("generic")}
          {pill("brainstorm")}
        </div>
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
    </form>
  );
}
