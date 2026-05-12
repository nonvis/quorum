import { useState, useEffect, useRef } from "react";
import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";
import type { Conversation, Task } from "../types";
import { fetchConversation, updateBudget } from "../api";
import { StateBadge } from "./StateBadge";
import { TaskTimeline } from "./TaskTimeline";
import { RespondControls } from "./RespondControls";

// Quorum protocol blocks are wrapped in fenced code blocks (```HANDOFF, etc.)
// which Markdown renders as literal code — hiding the inner markdown
// formatting. Pre-extract HANDOFF blocks so we can render their prompt body
// with full Markdown support.
type Segment =
  | { kind: "prose"; text: string }
  | { kind: "handoff"; to: string; prompt: string };

function parseSegments(text: string): Segment[] {
  const segments: Segment[] = [];
  const re = /```HANDOFF\s*\n([\s\S]*?)\n?```/g;
  let last = 0;
  let m: RegExpExecArray | null;
  while ((m = re.exec(text)) !== null) {
    if (m.index > last) {
      const prose = text.slice(last, m.index);
      if (prose.trim().length > 0) segments.push({ kind: "prose", text: prose });
    }
    const inner = m[1];
    const lines = inner.split("\n");
    let to = "";
    let body = "";
    let collecting = false;
    let indent = 0;
    for (let i = 0; i < lines.length; i++) {
      const ln = lines[i];
      if (!collecting) {
        const toMatch = ln.match(/^\s*to:\s*(.+)$/);
        if (toMatch && !to) {
          to = toMatch[1].trim();
          continue;
        }
        const pMatch = ln.match(/^\s*prompt:\s*(.*)$/);
        if (pMatch) {
          const rest = pMatch[1].trim();
          if (rest === "|") {
            // Canonical multi-line form: 2-space indented body follows
            collecting = true;
            indent = 2;
          } else {
            // Plain form (single-line or pre-fix multi-line) — start body
            // with whatever's after `prompt:` on the same line, then keep
            // appending subsequent lines verbatim.
            body = rest;
            collecting = true;
            indent = 0;
          }
        }
      } else {
        let line = ln;
        if (indent > 0 && line.startsWith(" ".repeat(indent))) {
          line = line.slice(indent);
        }
        body += (body.length > 0 ? "\n" : "") + line;
      }
    }
    segments.push({ kind: "handoff", to, prompt: body });
    last = re.lastIndex;
  }
  if (last < text.length) {
    const tail = text.slice(last);
    if (tail.trim().length > 0) segments.push({ kind: "prose", text: tail });
  }
  return segments;
}

function HandoffBox({ to, prompt }: { to: string; prompt: string }) {
  return (
    <div className="my-2 border border-zinc-700 rounded overflow-hidden">
      <div className="bg-zinc-800/70 px-3 py-1.5 text-xs text-zinc-400 border-b border-zinc-700 flex items-center gap-2">
        <span className="text-zinc-500">→ HANDOFF</span>
        <span className="text-emerald-400 font-mono">{to}</span>
      </div>
      <div className="md-content p-3 bg-zinc-950">
        <ReactMarkdown remarkPlugins={[remarkGfm]}>{prompt}</ReactMarkdown>
      </div>
    </div>
  );
}

function TaskItem({
  task,
  isCollapsed,
  onToggle,
}: {
  task: Task;
  isCollapsed: boolean;
  onToggle: () => void;
}) {
  const bodyRef = useRef<HTMLDivElement>(null);
  // null = default height (16rem); number = custom px height ("fitted")
  const [customHeightPx, setCustomHeightPx] = useState<number | null>(null);

  const MAX_PX = 768; // 48rem ceiling
  const handleFit = () => {
    if (customHeightPx !== null) {
      setCustomHeightPx(null);
      return;
    }
    if (bodyRef.current) {
      const target = Math.min(bodyRef.current.scrollHeight + 16, MAX_PX);
      setCustomHeightPx(target);
    }
  };

  const segments = task.result ? parseSegments(task.result) : [];

  return (
    <div className="text-sm">
      <div
        className="flex items-center justify-between mb-1 cursor-pointer hover:bg-zinc-800/40 -mx-1 px-1 rounded select-none"
        onClick={onToggle}
      >
        <span className="text-zinc-300 font-medium flex items-center gap-2">
          <span className="text-zinc-500 text-xs w-3 inline-block">
            {isCollapsed ? "▶" : "▼"}
          </span>
          {task.agent}
        </span>
        <div className="flex gap-3 text-zinc-500 text-xs">
          {task.token_in != null && <span>{task.token_in.toLocaleString()} in</span>}
          {task.token_out != null && <span>{task.token_out.toLocaleString()} out</span>}
          {task.cost != null && <span className="font-mono">${task.cost.toFixed(2)}</span>}
        </div>
      </div>
      {!isCollapsed && task.result && (
        <>
          <div
            ref={bodyRef}
            className="md-content bg-zinc-950 rounded-t p-3 overflow-auto"
            style={{
              height: customHeightPx !== null ? `${customHeightPx}px` : "16rem",
              minHeight: "4rem",
              maxHeight: `${MAX_PX}px`,
            }}
          >
            {segments.map((seg, i) =>
              seg.kind === "prose" ? (
                <ReactMarkdown key={i} remarkPlugins={[remarkGfm]}>
                  {seg.text}
                </ReactMarkdown>
              ) : (
                <HandoffBox key={i} to={seg.to} prompt={seg.prompt} />
              )
            )}
          </div>
          <div className="bg-zinc-900 border-t border-zinc-800 rounded-b flex justify-end">
            <button
              type="button"
              onClick={handleFit}
              onDoubleClick={handleFit}
              className="text-zinc-400 hover:text-zinc-100 text-xs px-3 py-1 font-mono"
              title={
                customHeightPx !== null
                  ? "Click to shrink to default (single or double click)"
                  : "Click to fit content (single or double click)"
              }
            >
              {customHeightPx !== null ? "⤡ Shrink" : "⤢ Fit"}
            </button>
          </div>
        </>
      )}
      {!isCollapsed && task.error && (
        <pre className="text-red-400 text-xs bg-red-950/30 rounded p-2 mt-1 whitespace-pre-wrap">
          {task.error}
        </pre>
      )}
    </div>
  );
}

export function ConversationCard({
  conversation,
  onAction,
}: {
  conversation: Conversation;
  onAction: () => void;
}) {
  const [expanded, setExpanded] = useState(false);
  const [tasks, setTasks] = useState<Task[]>([]);
  const [collapsedTaskIds, setCollapsedTaskIds] = useState<Set<number>>(new Set());
  const [budgetInput, setBudgetInput] = useState("");
  const c = conversation;

  const toggleTask = (id: number) => {
    setCollapsedTaskIds((prev) => {
      const next = new Set(prev);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });
  };

  useEffect(() => {
    fetchConversation(c.id).then((data) => setTasks(data.tasks));
  }, [c.id, c.state, c.round]);

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
            <TaskItem
              key={task.id}
              task={task}
              isCollapsed={collapsedTaskIds.has(task.id)}
              onToggle={() => toggleTask(task.id)}
            />
          ))}
        </div>
      )}
    </div>
  );
}
