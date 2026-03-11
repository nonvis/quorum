import { useState, useEffect } from "react";
import type { Conversation, Task } from "../types";
import { fetchConversation } from "../api";
import { StateBadge } from "./StateBadge";
import { TaskTimeline } from "./TaskTimeline";
import { GateControls } from "./GateControls";

export function ConversationCard({
  conversation,
  onAction,
}: {
  conversation: Conversation;
  onAction: () => void;
}) {
  const [expanded, setExpanded] = useState(false);
  const [tasks, setTasks] = useState<Task[]>([]);
  const c = conversation;

  useEffect(() => {
    if (expanded) {
      fetchConversation(c.id).then((data) => setTasks(data.tasks));
    }
  }, [expanded, c.id, c.state]);

  // Auto-expand if in approved state (needs user action)
  const showGate = c.state === "approved";

  // Find the thinker's result for proposal display
  const thinkerTask = tasks.find((t) => t.task_type === "think" && t.status === "done");
  const proposalText = thinkerTask?.result;

  return (
    <div
      className={`border rounded-lg p-4 cursor-pointer transition-colors ${
        showGate
          ? "border-orange-600 bg-orange-950/20"
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
            {c.paused_reason && (
              <span className="text-amber-400 text-xs truncate">({c.paused_reason})</span>
            )}
          </div>
          <p className="text-white truncate">{c.goal}</p>
        </div>
        <div className="text-right text-sm text-zinc-400 shrink-0">
          <div className="font-mono">${c.spent_usd.toFixed(2)}</div>
          <div className="text-xs">{c.pipeline}</div>
        </div>
      </div>

      {/* Task timeline (always visible) */}
      {tasks.length > 0 && (
        <div className="mt-3">
          <TaskTimeline tasks={tasks} />
        </div>
      )}

      {/* Gate controls */}
      {showGate && (
        <div
          className="mt-3 flex items-center justify-between"
          onClick={(e) => e.stopPropagation()}
        >
          <span className="text-orange-400 text-sm">Awaiting your decision</span>
          <GateControls conversationId={c.id} onAction={onAction} />
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
                  {task.agent} ({task.task_type})
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

      {/* Proposal preview (when approved, not expanded) */}
      {showGate && !expanded && proposalText && (
        <div className="mt-2" onClick={(e) => e.stopPropagation()}>
          <pre className="text-zinc-400 text-xs bg-zinc-950 rounded p-2 max-h-32 overflow-y-auto whitespace-pre-wrap">
            {proposalText.slice(0, 500)}
            {proposalText.length > 500 ? "..." : ""}
          </pre>
        </div>
      )}
    </div>
  );
}
