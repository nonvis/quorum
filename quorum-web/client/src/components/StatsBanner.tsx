import type { Stats, ProjectConfig } from "../types";

export function StatsBanner({
  stats,
  config,
}: {
  stats: Stats | null;
  config: ProjectConfig | null;
}) {
  if (!stats) return null;
  return (
    <div className="flex items-center justify-between px-6 py-3 bg-zinc-900 border-b border-zinc-800">
      <div className="flex items-center gap-4">
        <h1 className="text-lg font-semibold text-white">Quorum</h1>
        {config?.target_dir && (
          <span className="text-xs font-mono text-zinc-500 bg-zinc-800 px-2 py-0.5 rounded">
            {config.target_dir}
          </span>
        )}
        {config?.pipeline && (
          <span className="text-xs text-zinc-500">
            {config.pipeline}
          </span>
        )}
      </div>
      <div className="flex gap-6 text-sm text-zinc-400">
        <span>{stats.total_conversations} conversations</span>
        <span>{stats.active_conversations} active</span>
        <span className="text-white font-mono">${stats.total_cost_usd.toFixed(2)}</span>
      </div>
    </div>
  );
}
