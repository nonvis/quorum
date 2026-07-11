import { useState, useEffect } from "react";
import type { Agent, BudgetInfo } from "../types";
import { fetchBudget } from "../api";
import { roleColor, MODE } from "../lib/theme";
import { AgentCreateForm } from "./AgentCreateForm";

function SectionLabel({ children }: { children: React.ReactNode }) {
  return (
    <div className="mx-2 mb-2.5 font-mono text-[10.5px] font-semibold tracking-[0.14em] text-faint">
      {children}
    </div>
  );
}

function AgentRow({ agent, onClick }: { agent: Agent; onClick: () => void }) {
  return (
    <button
      onClick={onClick}
      title={agent.description}
      className="flex items-center gap-2.5 rounded-[10px] px-2.5 py-2 text-left hover:bg-chip"
    >
      <span
        className="h-2 w-2 flex-shrink-0 rounded-full"
        style={{ background: roleColor(agent.role), boxShadow: `0 0 8px ${roleColor(agent.role)}80` }}
      />
      <span className="truncate font-mono text-[12.5px] text-ink">{agent.name}</span>
      {agent.skill_file && (
        <span className="text-[10px] text-brand" title={`skill: ${agent.skill_file}`}>✦</span>
      )}
      <span className="ml-auto flex-shrink-0 text-[11px] text-faint">{agent.role}</span>
    </button>
  );
}

function budgetBarColor(pct: number): string {
  if (pct > 80) return "#e3a45c";
  if (pct > 60) return "#d8c06a";
  return "#63b3a6";
}

function fmtRemaining(min: number): string {
  const h = Math.floor(min / 60);
  const m = min % 60;
  return h > 0 ? `${h}h ${m}m` : `${m}m`;
}

export function Sidebar({
  agents,
  onAgentClick,
  onAgentCreated,
  onOpenBudget,
}: {
  agents: Agent[];
  onAgentClick: (id: string) => void;
  onAgentCreated: () => void;
  onOpenBudget: () => void;
}) {
  const [budget, setBudget] = useState<BudgetInfo | null>(null);

  useEffect(() => {
    fetchBudget().then(setBudget);
    const id = setInterval(() => fetchBudget().then(setBudget), 30000);
    return () => clearInterval(id);
  }, []);

  const pct = budget && budget.budget_usd > 0 ? Math.min(100, (budget.spent_usd / budget.budget_usd) * 100) : 0;

  return (
    <div className="flex flex-col gap-7 border-r border-line bg-rail px-4 py-[22px]">
      {/* Agents */}
      <div>
        <SectionLabel>AGENTS</SectionLabel>
        <div className="flex flex-col gap-0.5">
          {agents.map((a) => (
            <AgentRow key={a.id} agent={a} onClick={() => onAgentClick(a.id)} />
          ))}
          {agents.length === 0 && (
            <p className="px-2.5 text-[11px] text-dim">No agents yet.</p>
          )}
        </div>
        <div className="mt-2 px-1">
          <AgentCreateForm onCreated={onAgentCreated} />
        </div>
      </div>

      {/* Budget window */}
      <div>
        <button onClick={onOpenBudget} className="mb-2.5 flex w-full items-center px-2 text-left">
          <span className="font-mono text-[10.5px] font-semibold tracking-[0.14em] text-faint">
            BUDGET WINDOW
          </span>
          <span className="ml-auto text-[11px] text-dim hover:text-muted">adjust →</span>
        </button>
        {budget ? (
          <div className="px-2">
            <div className="mb-2 flex items-baseline justify-between">
              <span className="font-mono text-[12.5px] text-ink">${budget.spent_usd.toFixed(2)}</span>
              <span className="text-[11px] text-faint">of ${budget.budget_usd.toFixed(2)}</span>
            </div>
            <div className="h-1.5 overflow-hidden rounded-full bg-line">
              <div
                className="h-full rounded-full transition-[width] duration-1000"
                style={{ width: `${pct}%`, background: budgetBarColor(pct) }}
              />
            </div>
            <div className="mt-2 text-[11px] text-faint">
              {budget.is_expired ? "window expired — resets next task" : `resets in ${fmtRemaining(budget.remaining_minutes)}`}
            </div>
          </div>
        ) : (
          <div className="px-2 text-[11px] text-dim">—</div>
        )}
      </div>

      {/* Modes legend */}
      <div>
        <SectionLabel>MODES</SectionLabel>
        <div className="flex flex-col gap-2.5 px-2">
          {(["generic", "brainstorm"] as const).map((k) => (
            <div key={k} className="flex items-baseline gap-2">
              <span className="font-mono text-[12px]" style={{ color: MODE[k].color }}>
                {MODE[k].icon} {MODE[k].label}
              </span>
              <span className="text-[11px] text-faint">{MODE[k].hint}</span>
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}
