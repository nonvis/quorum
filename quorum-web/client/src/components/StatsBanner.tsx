import type { Stats, ProjectConfig } from "../types";

export function StatsBanner({
  stats,
  config,
  onSettingsClick,
  projectName,
}: {
  stats: Stats | null;
  config: ProjectConfig | null;
  onSettingsClick?: () => void;
  projectName?: string | null;
}) {
  if (!stats) return null;
  return (
    <div className="flex items-center justify-between px-6 py-3 bg-zinc-900 border-b border-zinc-800">
      <div className="flex items-center gap-4">
        <h1 className="text-lg font-semibold text-white">Quorum</h1>
        {projectName && (
          <span className="text-sm font-mono text-blue-400">
            {projectName}
          </span>
        )}
        {config?.daemon.target_dir && (
          <span className="text-xs font-mono text-zinc-500 bg-zinc-800 px-2 py-0.5 rounded">
            {config.daemon.target_dir}
          </span>
        )}
        {config?.conversations.leader && (
          <span className="text-xs text-zinc-500">
            leader: {config.conversations.leader}
          </span>
        )}
      </div>
      <div className="flex gap-6 text-sm text-zinc-400 items-center">
        <span>{stats.total_conversations} conversations</span>
        <span>{stats.active_conversations} active</span>
        <span className="text-white font-mono">
          ${stats.total_cost_usd.toFixed(2)} total
        </span>
        {onSettingsClick && (
          <button
            onClick={onSettingsClick}
            className="text-zinc-500 hover:text-zinc-300 transition-colors"
            title="Project Settings"
          >
            <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 20 20" fill="currentColor" className="w-5 h-5">
              <path fillRule="evenodd" d="M7.84 1.804A1 1 0 018.82 1h2.36a1 1 0 01.98.804l.331 1.652a6.993 6.993 0 011.929 1.115l1.598-.54a1 1 0 011.186.447l1.18 2.044a1 1 0 01-.205 1.251l-1.267 1.113a7.047 7.047 0 010 2.228l1.267 1.113a1 1 0 01.206 1.25l-1.18 2.045a1 1 0 01-1.187.447l-1.598-.54a6.993 6.993 0 01-1.929 1.115l-.33 1.652a1 1 0 01-.98.804H8.82a1 1 0 01-.98-.804l-.331-1.652a6.993 6.993 0 01-1.929-1.115l-1.598.54a1 1 0 01-1.186-.447l-1.18-2.044a1 1 0 01.205-1.251l1.267-1.114a7.05 7.05 0 010-2.227L1.821 7.773a1 1 0 01-.206-1.25l1.18-2.045a1 1 0 011.187-.447l1.598.54A6.993 6.993 0 017.51 3.456l.33-1.652zM10 13a3 3 0 100-6 3 3 0 000 6z" clipRule="evenodd" />
            </svg>
          </button>
        )}
      </div>
    </div>
  );
}
