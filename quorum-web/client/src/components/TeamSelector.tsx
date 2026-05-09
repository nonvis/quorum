import type { Team } from "../types";

export function TeamSelector({
  teams,
  selected,
  onSelect,
}: {
  teams: Team[];
  selected: string | null;
  onSelect: (teamId: string) => void;
}) {
  if (teams.length === 0) return null;

  return (
    <div className="px-6 py-2 flex items-center gap-2 border-b border-zinc-800/50">
      <span className="text-xs text-zinc-400 uppercase tracking-wide font-semibold w-12">Team:</span>
      {teams.map((team) => (
        <button
          key={team.id}
          onClick={() => onSelect(team.id)}
          title={team.default_path.join(" \u2192 ")}
          className={`px-3 py-1 rounded-full text-xs font-medium cursor-pointer ${
            selected === team.id
              ? "bg-blue-600 text-white"
              : "bg-zinc-800 text-zinc-400 hover:bg-zinc-700"
          }`}
        >
          {team.name}
        </button>
      ))}
    </div>
  );
}
