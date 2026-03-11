import { useState } from "react";
import { startConversation } from "../api";

export function PromptInput({ onSubmit }: { onSubmit: () => void }) {
  const [goal, setGoal] = useState("");
  const [autoApprove, setAutoApprove] = useState(false);
  const [loading, setLoading] = useState(false);

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!goal.trim() || loading) return;
    setLoading(true);
    try {
      await startConversation(goal.trim(), autoApprove);
      setGoal("");
      onSubmit();
    } finally {
      setLoading(false);
    }
  };

  return (
    <form onSubmit={handleSubmit} className="px-6 py-4">
      <div className="flex gap-3 items-center">
        <input
          type="text"
          value={goal}
          onChange={(e) => setGoal(e.target.value)}
          placeholder="What should I build?"
          className="flex-1 px-4 py-2 bg-zinc-800 border border-zinc-700 rounded-lg text-white placeholder-zinc-500 focus:outline-none focus:border-zinc-500"
          disabled={loading}
        />
        <label className="flex items-center gap-2 text-sm text-zinc-400 cursor-pointer select-none">
          <input
            type="checkbox"
            checked={autoApprove}
            onChange={(e) => setAutoApprove(e.target.checked)}
            className="rounded"
          />
          Auto-approve
        </label>
        <button
          type="submit"
          disabled={!goal.trim() || loading}
          className="px-4 py-2 bg-blue-600 text-white rounded-lg font-medium hover:bg-blue-500 disabled:opacity-50 disabled:cursor-not-allowed"
        >
          {loading ? "..." : "Go"}
        </button>
      </div>
    </form>
  );
}
