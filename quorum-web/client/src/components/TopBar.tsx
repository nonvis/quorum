import { ProjectSwitcher } from "./ProjectSwitcher";

// Top bar: identity, project switcher, daemon health, needs-you alert, and
// settings. Deliberately NO all-time total cost — what matters is the resetting
// budget window (shown in the sidebar), not a cumulative number.
export function TopBar({
  projectCurrent,
  projectRecent,
  onProjectSelect,
  daemonRunning,
  needsCount,
  convCount,
  onSettingsClick,
}: {
  projectCurrent: string | null;
  projectRecent: string[];
  onProjectSelect: (path: string) => void;
  daemonRunning: boolean;
  needsCount: number;
  convCount: number;
  onSettingsClick: () => void;
}) {
  return (
    <div className="flex h-[58px] flex-shrink-0 items-center gap-3.5 border-b border-line bg-topbar px-6">
      <div className="flex items-center gap-2.5">
        <span
          className="inline-block h-2.5 w-2.5 rotate-45 rounded-[3px]"
          style={{ background: "linear-gradient(135deg,#e3a45c,#c97f3f)" }}
        />
        <span className="text-[16px] font-bold tracking-[0.01em] text-ink-bright">quorum</span>
      </div>

      <ProjectSwitcher current={projectCurrent} recent={projectRecent} onSelect={onProjectSelect} />

      {projectCurrent &&
        (daemonRunning ? (
          <span className="inline-flex items-center gap-[7px] font-mono text-[11px] text-done">
            <span className="relative inline-block h-[7px] w-[7px]">
              <span className="q-ping absolute inset-0 rounded-full bg-done" />
              <span className="absolute inset-0 rounded-full bg-done" />
            </span>
            daemon live
          </span>
        ) : (
          <span className="inline-flex items-center gap-[7px] font-mono text-[11px] text-paused">
            <span className="inline-block h-[7px] w-[7px] rounded-full bg-paused" />
            daemon idle
          </span>
        ))}

      <span className="flex-1" />

      {needsCount > 0 && (
        <span
          className="inline-flex items-center gap-[7px] rounded-full px-2.5 py-1 font-mono text-[11px] font-semibold tracking-[0.05em] text-brand"
          style={{ background: "rgba(227,164,92,0.10)", border: "1px solid rgba(227,164,92,0.3)" }}
        >
          ◆ {needsCount === 1 ? "1 needs your input" : `${needsCount} need your input`}
        </span>
      )}

      {projectCurrent && <span className="text-xs text-faint">{convCount} conversations</span>}

      <button
        onClick={onSettingsClick}
        className="text-faint transition-colors hover:text-ink"
        title="Project settings"
      >
        <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 20 20" fill="currentColor" className="h-5 w-5">
          <path
            fillRule="evenodd"
            d="M7.84 1.804A1 1 0 018.82 1h2.36a1 1 0 01.98.804l.331 1.652a6.993 6.993 0 011.929 1.115l1.598-.54a1 1 0 011.186.447l1.18 2.044a1 1 0 01-.205 1.251l-1.267 1.113a7.047 7.047 0 010 2.228l1.267 1.113a1 1 0 01.206 1.25l-1.18 2.045a1 1 0 01-1.187.447l-1.598-.54a6.993 6.993 0 01-1.929 1.115l-.33 1.652a1 1 0 01-.98.804H8.82a1 1 0 01-.98-.804l-.331-1.652a6.993 6.993 0 01-1.929-1.115l-1.598.54a1 1 0 01-1.186-.447l-1.18-2.044a1 1 0 01.205-1.251l1.267-1.114a7.05 7.05 0 010-2.227L1.821 7.773a1 1 0 01-.206-1.25l1.18-2.045a1 1 0 011.187-.447l1.598.54A6.993 6.993 0 017.51 3.456l.33-1.652zM10 13a3 3 0 100-6 3 3 0 000 6z"
            clipRule="evenodd"
          />
        </svg>
      </button>
    </div>
  );
}
