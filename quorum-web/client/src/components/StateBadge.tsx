const stateColors: Record<string, string> = {
  active: "bg-blue-600",
  waiting_for_human: "bg-orange-600",
  done: "bg-green-600",
  closed: "bg-red-600",
  paused: "bg-amber-600",
};

export function StateBadge({ state }: { state: string }) {
  return (
    <span
      className={`px-2 py-0.5 rounded text-xs font-medium text-white ${stateColors[state] || "bg-zinc-600"}`}
    >
      {state.replace(/_/g, " ").toUpperCase()}
    </span>
  );
}
