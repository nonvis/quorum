const stateColors: Record<string, string> = {
  init: "bg-zinc-600",
  thinking: "bg-yellow-600",
  approved: "bg-orange-600",
  executing: "bg-blue-600",
  reviewing: "bg-purple-600",
  done: "bg-green-600",
  closed: "bg-red-600",
  paused: "bg-amber-600",
};

export function StateBadge({ state }: { state: string }) {
  return (
    <span
      className={`px-2 py-0.5 rounded text-xs font-medium text-white ${stateColors[state] || "bg-zinc-600"}`}
    >
      {state.toUpperCase()}
    </span>
  );
}
