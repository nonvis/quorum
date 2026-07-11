import { useState, useEffect } from "react";
import type { BudgetInfo, AgentCost } from "../types";
import { fetchBudget, fetchAgentCosts, updateWindowBudget } from "../api";

function fmtRemaining(min: number): string {
  const h = Math.floor(min / 60);
  const m = min % 60;
  return h > 0 ? `${h}h ${m}m` : `${m}m`;
}

function barColor(pct: number): string {
  if (pct > 85) return "#c98b81";
  if (pct > 60) return "#e3a45c";
  return "#63b3a6";
}

// Full budget controls (window sync + budget change + per-agent cost table),
// opened from the sidebar's compact window bar.
export function BudgetSheet({ onClose }: { onClose: () => void }) {
  const [budget, setBudget] = useState<BudgetInfo | null>(null);
  const [agents, setAgents] = useState<AgentCost[]>([]);
  const [syncMinutes, setSyncMinutes] = useState("");
  const [newBudget, setNewBudget] = useState("");
  const [loading, setLoading] = useState(false);

  const load = async () => {
    const [b, a] = await Promise.all([fetchBudget(), fetchAgentCosts()]);
    setBudget(b);
    setAgents(a);
  };

  useEffect(() => {
    load();
  }, []);

  const handleSync = async () => {
    const mins = parseInt(syncMinutes, 10);
    if (isNaN(mins) || mins < 0) return;
    setLoading(true);
    await updateWindowBudget(undefined, mins);
    await load();
    setLoading(false);
    setSyncMinutes("");
  };

  const handleBudget = async () => {
    const val = parseFloat(newBudget);
    if (isNaN(val) || val <= 0) return;
    setLoading(true);
    await updateWindowBudget(val, undefined);
    await load();
    setLoading(false);
    setNewBudget("");
  };

  const pct = budget && budget.budget_usd > 0 ? Math.min(100, (budget.spent_usd / budget.budget_usd) * 100) : 0;

  return (
    <div
      className="q-fade fixed inset-0 z-50 flex justify-end"
      style={{ background: "rgba(11,9,15,0.55)", backdropFilter: "blur(3px)" }}
      onClick={onClose}
    >
      <div
        onClick={(e) => e.stopPropagation()}
        className="q-slide flex h-full w-96 flex-col bg-sheet"
        style={{ borderLeft: "1px solid #322d3c" }}
      >
        <div className="flex flex-shrink-0 items-center justify-between border-b border-line px-5 py-4">
          <div>
            <h2 className="text-sm font-semibold text-ink-bright">Budget window</h2>
            <span className="text-xs text-faint">rolling {budget?.window_hours ?? 5}h window</span>
          </div>
          <button
            onClick={onClose}
            className="h-7 w-7 rounded-lg border border-line-edge text-[13px] text-muted hover:border-line-dash hover:text-ink"
          >
            ✕
          </button>
        </div>

        {budget && (
          <div className="flex-1 overflow-y-auto px-5 py-4">
            {/* bar */}
            <div className="mb-1 flex items-center justify-between text-xs text-muted">
              <span className="font-mono">
                ${budget.spent_usd.toFixed(2)} / ${budget.budget_usd.toFixed(2)}
              </span>
              <span>
                {budget.is_expired ? (
                  <span className="text-brand">window expired — resets next task</span>
                ) : (
                  `${fmtRemaining(budget.remaining_minutes)} remaining`
                )}
              </span>
            </div>
            <div className="h-2 overflow-hidden rounded-full bg-chip">
              <div
                className="h-full rounded-full transition-[width] duration-1000"
                style={{ width: `${pct}%`, background: barColor(pct) }}
              />
            </div>

            {/* controls */}
            <div className="mt-5 space-y-4">
              <div>
                <label className="mb-1 block text-[11px] text-faint">Sync timer — minutes left on claude.ai</label>
                <div className="flex gap-2">
                  <input
                    type="number"
                    min="0"
                    value={syncMinutes}
                    onChange={(e) => setSyncMinutes(e.target.value)}
                    onKeyDown={(e) => e.key === "Enter" && handleSync()}
                    placeholder="300"
                    className="w-24 rounded-lg border border-line-soft bg-field px-2.5 py-1.5 text-xs text-ink outline-none focus:border-faint"
                  />
                  <button
                    onClick={handleSync}
                    disabled={loading || !syncMinutes}
                    className="rounded-lg border border-line-dash bg-transparent px-3 py-1.5 text-xs font-semibold text-[#c9c3bd] hover:text-ink disabled:opacity-45"
                  >
                    Sync
                  </button>
                </div>
              </div>
              <div>
                <label className="mb-1 block text-[11px] text-faint">Change window budget (USD)</label>
                <div className="flex gap-2">
                  <input
                    type="number"
                    min="0"
                    step="10"
                    value={newBudget}
                    onChange={(e) => setNewBudget(e.target.value)}
                    onKeyDown={(e) => e.key === "Enter" && handleBudget()}
                    placeholder={budget.budget_usd.toString()}
                    className="w-24 rounded-lg border border-line-soft bg-field px-2.5 py-1.5 text-xs text-ink outline-none focus:border-faint"
                  />
                  <button
                    onClick={handleBudget}
                    disabled={loading || !newBudget}
                    className="rounded-lg bg-brand px-3 py-1.5 text-xs font-bold text-[#1a1410] hover:bg-brand-bright disabled:opacity-45"
                  >
                    Update
                  </button>
                </div>
              </div>
            </div>

            {/* agent cost table */}
            {agents.length > 0 && (
              <div className="mt-6">
                <div className="mb-2 font-mono text-[10.5px] tracking-[0.14em] text-faint">SPEND BY AGENT</div>
                <table className="w-full text-xs text-muted">
                  <thead>
                    <tr className="border-b border-line">
                      <th className="py-1 text-left font-medium text-faint">Agent</th>
                      <th className="py-1 text-right font-medium text-faint">Tasks</th>
                      <th className="py-1 text-right font-medium text-faint">Total</th>
                      <th className="py-1 text-right font-medium text-faint">Avg</th>
                    </tr>
                  </thead>
                  <tbody>
                    {agents.map((a) => (
                      <tr key={a.agent} className="border-b border-line-soft/60">
                        <td className="py-1 font-mono text-ink">{a.agent}</td>
                        <td className="py-1 text-right">{a.tasks}</td>
                        <td className="py-1 text-right font-mono">${a.total_cost.toFixed(2)}</td>
                        <td className="py-1 text-right font-mono">${a.avg_cost.toFixed(2)}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            )}
          </div>
        )}
      </div>
    </div>
  );
}
