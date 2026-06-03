import type { Agent } from "../types";

const ROLE_COLORS: Record<string, string> = {
  leader: "bg-purple-500",
  thinker: "bg-blue-500",
  doer: "bg-green-500",
  evaluator: "bg-indigo-500",
};

const ROLE_INITIALS: Record<string, string> = {
  leader: "L",
  thinker: "T",
  doer: "D",
  evaluator: "E",
};

function AgentBadge({ agent, dimmed, onClick }: { agent: Agent; dimmed: boolean; onClick?: () => void }) {
  const dotColor = ROLE_COLORS[agent.role] ?? "bg-zinc-500";
  const initial = ROLE_INITIALS[agent.role] ?? "?";
  const Wrapper = onClick ? "button" : "span";

  return (
    <Wrapper
      onClick={onClick}
      className={`inline-flex items-center gap-1 text-xs ${dimmed ? "opacity-50" : ""} ${onClick ? "cursor-pointer hover:bg-zinc-800 rounded px-1 -mx-1" : ""}`}
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
    </Wrapper>
  );
}

export function AgentRoster({
  agents,
  onAgentClick,
}: {
  agents: Agent[];
  onAgentClick?: (agentId: string) => void;
}) {
  if (agents.length === 0) return null;

  return (
    <div className="px-6 py-1 flex items-center gap-2 flex-wrap">
      <span className="text-xs text-zinc-500">Agents:</span>
      {agents.map((agent) => (
        <AgentBadge key={agent.id} agent={agent} dimmed={false} onClick={() => onAgentClick?.(agent.id)} />
      ))}
    </div>
  );
}
