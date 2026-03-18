import type { Agent } from "../types";

const ROLE_COLORS: Record<string, string> = {
  leader: "bg-purple-500",
  thinker: "bg-blue-500",
  doer: "bg-green-500",
  reviewer: "bg-yellow-500",
  scribe: "bg-cyan-500",
  librarian: "bg-pink-500",
};

const ROLE_INITIALS: Record<string, string> = {
  leader: "L",
  thinker: "T",
  doer: "D",
  reviewer: "R",
  scribe: "S",
  librarian: "Lb",
};

function AgentBadge({ agent, dimmed }: { agent: Agent; dimmed: boolean }) {
  const dotColor = ROLE_COLORS[agent.role] ?? "bg-zinc-500";
  const initial = ROLE_INITIALS[agent.role] ?? "?";

  return (
    <span
      className={`inline-flex items-center gap-1 text-xs ${dimmed ? "opacity-50" : ""}`}
      title={agent.description}
    >
      <span className={`w-2 h-2 rounded-full ${dotColor}`} />
      <span className={dimmed ? "text-zinc-600" : "text-zinc-300"}>
        {agent.name}
      </span>
      <span className={dimmed ? "text-zinc-700" : "text-zinc-500"}>
        ({initial})
      </span>
      {agent.skill_file && (
        <span className="text-amber-500 text-[10px] ml-0.5" title={`Skill: ${agent.skill_file}`}>&#9733;</span>
      )}
    </span>
  );
}

export function AgentRoster({
  agents,
  teamPath,
}: {
  agents: Agent[];
  teamPath: string[];
}) {
  if (agents.length === 0) return null;

  const agentMap = new Map(agents.map((a) => [a.id, a]));

  // Agents in teamPath order (only those that exist)
  const inPath = teamPath
    .map((id) => agentMap.get(id))
    .filter((a): a is Agent => a !== undefined);

  // Remaining agents not in teamPath
  const pathSet = new Set(teamPath);
  const outOfPath = agents.filter((a) => !pathSet.has(a.id));

  return (
    <div className="px-6 py-1 flex items-center gap-2 flex-wrap">
      <span className="text-xs text-zinc-500">Agents:</span>
      {inPath.map((agent, i) => (
        <span key={agent.id} className="inline-flex items-center gap-2">
          {i > 0 && <span className="text-zinc-600 text-xs">&rarr;</span>}
          <AgentBadge agent={agent} dimmed={false} />
        </span>
      ))}
      {outOfPath.map((agent) => (
        <AgentBadge key={agent.id} agent={agent} dimmed={true} />
      ))}
    </div>
  );
}
