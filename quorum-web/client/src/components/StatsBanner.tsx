import type { Stats } from "../types";

export function StatsBanner({ stats }: { stats: Stats | null }) {
  if (!stats) return null;
  return (
    <div className="flex items-center justify-between px-6 py-3 bg-zinc-900 border-b border-zinc-800">
      <h1 className="text-lg font-semibold text-white">Quorum</h1>
      <div className="flex gap-6 text-sm text-zinc-400">
        <span>{stats.total_conversations} conversations</span>
        <span>{stats.active_conversations} active</span>
        <span className="text-white font-mono">${stats.total_cost_usd.toFixed(2)}</span>
      </div>
    </div>
  );
}
