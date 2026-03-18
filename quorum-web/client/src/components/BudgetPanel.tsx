import { useState, useEffect } from "react";
import type { BudgetInfo, AgentCost } from "../types";
import { fetchBudget, fetchAgentCosts, updateWindowBudget } from "../api";

function formatTime(minutes: number): string {
  const h = Math.floor(minutes / 60);
  const m = minutes % 60;
  if (h > 0) return `${h}h ${m}m`;
  return `${m}m`;
}

function barColor(pct: number): string {
  if (pct > 85) return "bg-red-600";
  if (pct > 60) return "bg-yellow-600";
  return "bg-green-600";
}

export function BudgetPanel() {
  const [budget, setBudget] = useState<BudgetInfo | null>(null);
  const [agents, setAgents] = useState<AgentCost[]>([]);
  const [collapsed, setCollapsed] = useState(false);
  const [showSync, setShowSync] = useState(false);
  const [showBudgetEdit, setShowBudgetEdit] = useState(false);
  const [syncMinutes, setSyncMinutes] = useState("");
  const [newBudget, setNewBudget] = useState("");
  const [loading, setLoading] = useState(false);

  const loadData = async () => {
    const [b, a] = await Promise.all([fetchBudget(), fetchAgentCosts()]);
    setBudget(b);
    setAgents(a);
  };

  useEffect(() => {
    loadData();
    const interval = setInterval(() => {
      fetchBudget().then(setBudget);
    }, 30000);
    return () => clearInterval(interval);
  }, []);

  const handleSync = async () => {
    const mins = parseInt(syncMinutes);
    if (isNaN(mins) || mins < 0) return;
    setLoading(true);
    await updateWindowBudget(undefined, mins);
    await loadData();
    setLoading(false);
    setShowSync(false);
    setSyncMinutes("");
  };

  const handleBudgetUpdate = async () => {
    const val = parseFloat(newBudget);
    if (isNaN(val) || val <= 0) return;
    setLoading(true);
    await updateWindowBudget(val, undefined);
    await loadData();
    setLoading(false);
    setShowBudgetEdit(false);
    setNewBudget("");
  };

  if (!budget) return null;

  const pct = budget.budget_usd > 0
    ? Math.min(100, (budget.spent_usd / budget.budget_usd) * 100)
    : 0;

  return (
    <div className="bg-zinc-900/50 border border-zinc-800 rounded-lg mx-6 my-3 p-4">
      <button
        onClick={() => setCollapsed(!collapsed)}
        className="flex items-center justify-between w-full text-left"
      >
        <span className="text-sm font-medium text-zinc-300">Budget</span>
        <svg
          xmlns="http://www.w3.org/2000/svg"
          viewBox="0 0 20 20"
          fill="currentColor"
          className={`w-4 h-4 text-zinc-500 transition-transform ${collapsed ? "-rotate-90" : ""}`}
        >
          <path fillRule="evenodd" d="M5.23 7.21a.75.75 0 011.06.02L10 11.168l3.71-3.938a.75.75 0 111.08 1.04l-4.25 4.5a.75.75 0 01-1.08 0l-4.25-4.5a.75.75 0 01.02-1.06z" clipRule="evenodd" />
        </svg>
      </button>

      {!collapsed && (
        <div className="mt-3 space-y-3">
          {/* Progress bar */}
          <div>
            <div className="flex items-center justify-between text-xs text-zinc-400 mb-1">
              <span className="font-mono">
                ${budget.spent_usd.toFixed(2)} / ${budget.budget_usd.toFixed(2)}
              </span>
              <span>
                {budget.is_expired
                  ? <span className="text-amber-400">Window expired — resets on next task</span>
                  : formatTime(budget.remaining_minutes) + " remaining"
                }
              </span>
            </div>
            <div className="w-full bg-zinc-800 rounded-full h-2">
              <div
                className={`h-2 rounded-full transition-all ${barColor(pct)}`}
                style={{ width: `${pct}%` }}
              />
            </div>
          </div>

          {/* Action buttons */}
          <div className="flex gap-2">
            <button
              onClick={() => { setShowSync(!showSync); setShowBudgetEdit(false); }}
              className={`text-xs px-2 py-1 rounded border transition-colors ${
                showSync
                  ? "border-blue-600 text-blue-400 bg-blue-950/50"
                  : "border-zinc-700 text-zinc-400 hover:text-zinc-300 hover:border-zinc-600"
              }`}
            >
              Sync Timer
            </button>
            <button
              onClick={() => { setShowBudgetEdit(!showBudgetEdit); setShowSync(false); }}
              className={`text-xs px-2 py-1 rounded border transition-colors ${
                showBudgetEdit
                  ? "border-blue-600 text-blue-400 bg-blue-950/50"
                  : "border-zinc-700 text-zinc-400 hover:text-zinc-300 hover:border-zinc-600"
              }`}
            >
              Change Budget
            </button>
          </div>

          {/* Sync Timer input */}
          {showSync && (
            <div className="flex items-center gap-2">
              <span className="text-xs text-zinc-500">Minutes remaining on claude.ai:</span>
              <input
                type="number"
                min="0"
                value={syncMinutes}
                onChange={(e) => setSyncMinutes(e.target.value)}
                onKeyDown={(e) => e.key === "Enter" && handleSync()}
                className="w-20 bg-zinc-800 border border-zinc-700 rounded px-2 py-1 text-xs text-white focus:outline-none focus:border-blue-600"
                placeholder="300"
              />
              <button
                onClick={handleSync}
                disabled={loading}
                className="text-xs px-2 py-1 bg-blue-600 hover:bg-blue-500 rounded text-white disabled:opacity-50"
              >
                Sync
              </button>
            </div>
          )}

          {/* Change Budget input */}
          {showBudgetEdit && (
            <div className="flex items-center gap-2">
              <span className="text-xs text-zinc-500">$</span>
              <input
                type="number"
                min="0"
                step="10"
                value={newBudget}
                onChange={(e) => setNewBudget(e.target.value)}
                onKeyDown={(e) => e.key === "Enter" && handleBudgetUpdate()}
                className="w-24 bg-zinc-800 border border-zinc-700 rounded px-2 py-1 text-xs text-white focus:outline-none focus:border-blue-600"
                placeholder={budget.budget_usd.toString()}
              />
              <button
                onClick={handleBudgetUpdate}
                disabled={loading}
                className="text-xs px-2 py-1 bg-blue-600 hover:bg-blue-500 rounded text-white disabled:opacity-50"
              >
                Update
              </button>
            </div>
          )}

          {/* Agent cost table */}
          {agents.length > 0 && (
            <table className="w-full text-xs text-zinc-400">
              <thead>
                <tr className="border-b border-zinc-800">
                  <th className="text-left py-1 font-medium text-zinc-500">Agent</th>
                  <th className="text-right py-1 font-medium text-zinc-500">Tasks</th>
                  <th className="text-right py-1 font-medium text-zinc-500">Total ($)</th>
                  <th className="text-right py-1 font-medium text-zinc-500">Avg ($)</th>
                </tr>
              </thead>
              <tbody>
                {agents.map((a) => (
                  <tr key={a.agent} className="border-b border-zinc-800/50">
                    <td className="py-1 text-zinc-300 font-mono">{a.agent}</td>
                    <td className="py-1 text-right">{a.tasks}</td>
                    <td className="py-1 text-right font-mono">${a.total_cost.toFixed(2)}</td>
                    <td className="py-1 text-right font-mono">${a.avg_cost.toFixed(2)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          )}
        </div>
      )}
    </div>
  );
}
