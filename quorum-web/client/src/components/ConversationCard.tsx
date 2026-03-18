import { useState, useEffect } from "react";
import type { Conversation, Task } from "../types";
import { fetchConversation, updateBudget } from "../api";
import { StateBadge } from "./StateBadge";
import { TaskTimeline } from "./TaskTimeline";
import { RespondControls } from "./RespondControls";

export function ConversationCard({
  conversation,
  onAction,
}: {
  conversation: Conversation;
  onAction: () => void;
}) {
  const [expanded, setExpanded] = useState(false);
  const [tasks, setTasks] = useState<Task[]>([]);
  const [budgetInput, setBudgetInput] = useState("");
  const c = conversation;

  useEffect(() => {
    if (expanded) {
      fetchConversation(c.id).then((data) => setTasks(data.tasks));
    }
  }, [expanded, c.id, c.state]);

  const showRespond = c.state === "waiting_for_human";

  return (
    <div
      className={`border rounded-lg p-4 cursor-pointer transition-colors ${
        showRespond
          ? "border-blue-600 bg-blue-950/20"
          : "border-zinc-800 bg-zinc-900 hover:border-zinc-700"
      }`}
      onClick={() => setExpanded(!expanded)}
    >
      {/* Header */}
      <div className="flex items-start justify-between gap-4">
        <div className="flex-1 min-w-0">
          <div className="flex items-center gap-2 mb-1">
            <span className="text-zinc-500 text-sm">#{c.id}</span>
            <StateBadge state={c.state} />
            {c.state === "paused" && c.paused_reason && (
              <span className="text-amber-400 text-xs truncate">({c.paused_reason})</span>
            )}
          </div>
          <p className="text-white truncate">{c.goal}</p>
        </div>
        <div className="text-right text-sm text-zinc-400 shrink-0">
          <div className="font-mono">
            ${c.spent_usd.toFixed(2)}
            <span className="text-zinc-500"> / ${c.budget_usd.toFixed(2)}</span>
          </div>
          {c.current_agent && (
            <div className="text-xs">
              {c.state === "active" && <span className="text-blue-400">&rarr; </span>}
              {c.current_agent}
            </div>
          )}
        </div>
      </div>

      {/* Task timeline (always visible) */}
      {tasks.length > 0 && (
        <div className="mt-3">
          <TaskTimeline tasks={tasks} />
        </div>
      )}

      {/* Respond controls (waiting_for_human) */}
      {showRespond && (
        <div className="mt-3" onClick={(e) => e.stopPropagation()}>
          <RespondControls
            conversationId={c.id}
            leaderMessage={c.paused_reason}
            onAction={onAction}
          />
        </div>
      )}

      {/* Budget increase for paused conversations */}
      {c.state === "paused" && (
        <div
          className="mt-3 flex items-center gap-3"
          onClick={(e) => e.stopPropagation()}
        >
          <span className="text-amber-400 text-sm">Paused{c.paused_reason ? `: ${c.paused_reason}` : ""}</span>
          <div className="flex items-center gap-2 ml-auto">
            <span className="text-zinc-500 text-xs">Budget $</span>
            <input
              type="number"
              step="0.5"
              min={c.budget_usd}
              value={budgetInput}
              onChange={(e) => setBudgetInput(e.target.value)}
              placeholder={c.budget_usd.toFixed(2)}
              className="w-20 px-2 py-1 bg-zinc-800 border border-zinc-700 rounded text-white text-sm font-mono focus:outline-none focus:border-zinc-500"
            />
            <button
              onClick={async () => {
                const val = parseFloat(budgetInput);
                if (val > 0) {
                  await updateBudget(c.id, val);
                  setBudgetInput("");
                  onAction();
                }
              }}
              disabled={!budgetInput || parseFloat(budgetInput) <= 0}
              className="px-3 py-1 bg-amber-600 text-white rounded text-sm font-medium hover:bg-amber-500 disabled:opacity-50 disabled:cursor-not-allowed"
            >
              Increase & Resume
            </button>
          </div>
        </div>
      )}

      {/* Expanded detail */}
      {expanded && tasks.length > 0 && (
        <div
          className="mt-4 space-y-3 border-t border-zinc-800 pt-3"
          onClick={(e) => e.stopPropagation()}
        >
          {tasks.map((task) => (
            <div key={task.id} className="text-sm">
              <div className="flex items-center justify-between mb-1">
                <span className="text-zinc-300 font-medium">
                  {task.agent}
                </span>
                <div className="flex gap-3 text-zinc-500 text-xs">
                  {task.token_in != null && <span>{task.token_in.toLocaleString()} in</span>}
                  {task.token_out != null && <span>{task.token_out.toLocaleString()} out</span>}
                  {task.cost != null && (
                    <span className="font-mono">${task.cost.toFixed(2)}</span>
                  )}
                </div>
              </div>
              {task.result && (
                <pre className="text-zinc-400 text-xs bg-zinc-950 rounded p-2 overflow-x-auto max-h-48 overflow-y-auto whitespace-pre-wrap">
                  {task.result}
                </pre>
              )}
              {task.error && (
                <pre className="text-red-400 text-xs bg-red-950/30 rounded p-2">{task.error}</pre>
              )}
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
