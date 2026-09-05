import { useState, useEffect } from "react";
import type { Agent, BudgetInfo } from "../types";
import { fetchBudget, refreshKnowers } from "../api";
import { roleColor, MODE } from "../lib/theme";
import { AgentCreateForm } from "./AgentCreateForm";

// Web-first: `quorum knower refresh` gets a button (the post-build routine
// op). It spends tokens — four read-only brainstorm scans — so it asks for
// one confirming click, then the passes show up in the conversations list.
// The four lenses, in the daemon's refresh order (knower_refresh.h:74-77).
const KNOWER_LENSES = ["cartographer", "architect", "historian", "recap"] as const;

function KnowerRefresh() {
  const [confirming, setConfirming] = useState(false);
  const [busy, setBusy] = useState(false);
  const [note, setNote] = useState<{ text: string; error: boolean } | null>(null);
  const [lens, setLens] = useState<string>("all");
  const [parallel, setParallel] = useState(false);

  // --parallel is --all-only; the daemon rejects it for a single lens, so the
  // checkbox only exists for "All".
  const isAll = lens === "all";

  const go = async () => {
    setBusy(true);
    try {
      const res = await refreshKnowers(lens, isAll && parallel);
      if (res.started) {
        setNote({ text: res.note ?? "refresh started", error: false });
      } else {
        setNote({ text: res.error ?? "refresh failed", error: true });
      }
    } finally {
      setBusy(false);
      setConfirming(false);
    }
  };

  return (
    <div className="mt-1.5">
      {!confirming ? (
        <button
          onClick={() => {
            setNote(null);
            setConfirming(true);
          }}
          className="w-full rounded-[10px] border border-dashed border-line-dash px-2.5 py-1.5 text-left text-[11.5px] text-muted hover:text-ink"
          title="re-survey the codebase into the knower vaults (quorum knower refresh)"
        >
          ↻ Refresh knowers
        </button>
      ) : (
        <div className="rounded-[10px] border border-line-soft px-2.5 py-2">
          <p className="mb-1.5 text-[11px] leading-[1.5] text-muted">
            {isAll
              ? "Runs read-only scans for every knower — spends tokens."
              : `Runs a read-only ${lens} scan — spends tokens.`}
          </p>
          <label className="mb-1.5 flex items-center gap-1.5 text-[11px] text-muted">
            <span className="text-faint">lens</span>
            <select
              value={lens}
              onChange={(e) => setLens(e.target.value)}
              disabled={busy}
              className="flex-1 rounded-md border border-line-soft bg-field px-1.5 py-1 font-mono text-[11px] text-ink outline-none focus:border-faint"
            >
              <option value="all">All (4 passes)</option>
              {KNOWER_LENSES.map((k) => (
                <option key={k} value={k}>
                  {k}
                </option>
              ))}
            </select>
          </label>
          {isAll && (
            <label
              className="mb-1.5 flex items-center gap-1.5 text-[11px] text-muted"
              title="run the independent lenses concurrently (cartographer→architect stays ordered)"
            >
              <input
                type="checkbox"
                checked={parallel}
                onChange={(e) => setParallel(e.target.checked)}
                disabled={busy}
              />
              parallel
            </label>
          )}
          <div className="flex gap-1.5">
            <button
              onClick={go}
              disabled={busy}
              className="rounded-md px-2.5 py-1 text-[11.5px] font-bold text-[#171319] disabled:opacity-45"
              style={{ background: "#63b3a6" }}
            >
              {busy ? "…" : "Go"}
            </button>
            <button
              onClick={() => setConfirming(false)}
              disabled={busy}
              className="rounded-md border border-line-soft px-2.5 py-1 text-[11.5px] text-muted"
            >
              cancel
            </button>
          </div>
        </div>
      )}
      {note && (
        <p
          className="mt-1.5 px-1 text-[10.5px] leading-[1.5]"
          style={{ color: note.error ? "#c98b81" : "#85bd93" }}
        >
          {note.text}
        </p>
      )}
    </div>
  );
}

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
          <KnowerRefresh />
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
