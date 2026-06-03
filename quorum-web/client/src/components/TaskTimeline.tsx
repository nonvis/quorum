import type { Task } from "../types";

const statusIcon: Record<string, string> = {
  pending: "\u25CB",
  active: "\u25C9",
  done: "\u2713",
  failed: "\u2717",
};

const statusColor: Record<string, string> = {
  pending: "text-zinc-500",
  active: "text-blue-400",
  done: "text-green-400",
  failed: "text-red-400",
};

export function TaskTimeline({ tasks }: { tasks: Task[] }) {
  if (tasks.length === 0) return null;

  return (
    <div className="flex items-center gap-1 text-sm">
      {tasks.map((task, i) => (
        <span key={task.id} className="flex items-center gap-1">
          {i > 0 && <span className="text-zinc-600 mx-1">{"\u2192"}</span>}
          <span
            className={`${statusColor[task.status] || "text-zinc-500"}${
              task.status === "active" ? " animate-pulse font-medium" : ""
            }`}
          >
            {statusIcon[task.status] || "?"} {task.agent}
          </span>
          {task.cost != null && (
            <span className="text-zinc-600 text-xs">${task.cost.toFixed(2)}</span>
          )}
        </span>
      ))}
    </div>
  );
}
