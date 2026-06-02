import { useState } from "react";
import { startConversation, type ConversationMode } from "../api";

const MODES: { id: ConversationMode; label: string; hint: string }[] = [
  {
    id: "generic",
    label: "generic",
    hint: "Agents may modify the project",
  },
  {
    id: "brainstorm",
    label: "brainstorm",
    hint: "Read-only; scribe curates vault writes",
  },
];

const MODE_COLORS: Record<ConversationMode, { active: string; inactive: string }> = {
  generic:    { active: "bg-zinc-600 text-white",  inactive: "bg-zinc-800 text-zinc-400 hover:bg-zinc-700" },
  brainstorm: { active: "bg-amber-600 text-white", inactive: "bg-zinc-800 text-amber-400 hover:bg-zinc-700" },
};

export function PromptInput({
  onSubmit,
  busy,
}: {
  onSubmit: () => void;
  busy: boolean;
}) {
  const [goal, setGoal] = useState("");
  const [mode, setMode] = useState<ConversationMode>("generic");
  const [loading, setLoading] = useState(false);

  const disabled = loading || busy;

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

  const activeHint = MODES.find((m) => m.id === mode)?.hint ?? "";

  return (
    <form onSubmit={handleSubmit} className="px-6 py-4">
      <div className="flex items-center gap-2 mb-2">
        <span className="text-xs text-zinc-400 uppercase tracking-wide font-semibold w-12">Mode:</span>
        <div className="flex items-center gap-1">
          {MODES.map((m) => {
            const colors = MODE_COLORS[m.id];
            const isActive = mode === m.id;
            return (
              <button
                key={m.id}
                type="button"
                onClick={() => setMode(m.id)}
                disabled={disabled}
                title={m.hint}
                className={`px-3 py-0.5 text-xs rounded-full transition-colors disabled:opacity-50 disabled:cursor-not-allowed ${
                  isActive ? colors.active : colors.inactive
                }`}
              >
                {m.label}
              </button>
            );
          })}
        </div>
        <span className="text-xs text-zinc-600">{activeHint}</span>
      </div>
      <div className="flex gap-3 items-center">
        <input
          type="text"
          value={goal}
          onChange={(e) => setGoal(e.target.value)}
          placeholder={busy ? "Conversation in progress..." : "What should I build?"}
          className="flex-1 px-4 py-2 bg-zinc-800 border border-zinc-700 rounded-lg text-white placeholder-zinc-500 focus:outline-none focus:border-zinc-500 disabled:opacity-50"
          disabled={disabled}
        />
        <button
          type="submit"
          disabled={!goal.trim() || disabled}
          className="px-4 py-2 bg-blue-600 text-white rounded-lg font-medium hover:bg-blue-500 disabled:opacity-50 disabled:cursor-not-allowed"
        >
          {loading ? "..." : "Go"}
        </button>
      </div>
    </form>
  );
}
