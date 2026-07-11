// The autopilot pre-flight bookend: compose a goal-first flight plan, edit
// the drafted major tasks, generate SUPERVISOR.md, and copy the ONE terminal
// command that launches the flight. Honest by design — the web prepares and
// reviews flights; it cannot launch one (an interactive `claude --agent
// supervisor` session must be started by the operator).
import { useMemo, useState } from "react";
import type { Agent, PlanResult, PlanTask } from "../types";
import { submitFlightPlan } from "../api";
import { draftTasks } from "../lib/flightDraft";
import { modeOf, type ConversationMode } from "../lib/theme";

interface DraftTask {
  key: number;
  title: string;
  agent: string;
  slices: string; // one slice per line
  doneWhen: string;
}

const NIGHT = "#8fa9e8"; // flight accent — engine color, not a mode color

let nextKey = 1;

export function FlightComposer({
  mode,
  agents,
  onPlanned,
}: {
  mode: ConversationMode;
  agents: Agent[];
  onPlanned: () => void;
}) {
  const [goal, setGoal] = useState("");
  const [tasks, setTasks] = useState<DraftTask[]>([]);
  const [maxTasks, setMaxTasks] = useState("");
  const [drafted, setDrafted] = useState(false);
  const [submitting, setSubmitting] = useState(false);
  const [needsForce, setNeedsForce] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [result, setResult] = useState<PlanResult | null>(null);
  const [copied, setCopied] = useState(false);

  const m = modeOf(mode);
  const defaultAgent = useMemo(() => {
    const ids = agents.map((a) => a.id);
    return ids.includes("thinker") ? "thinker" : (ids[0] ?? "");
  }, [agents]);

  const draft = () => {
    const titles = draftTasks(goal);
    setTasks(
      titles.map((t) => ({ key: nextKey++, title: t, agent: defaultAgent, slices: "", doneWhen: "" })),
    );
    setDrafted(true);
    setResult(null);
    setNeedsForce(null);
    setError(null);
  };

  const update = (key: number, patch: Partial<DraftTask>) =>
    setTasks((ts) => ts.map((t) => (t.key === key ? { ...t, ...patch } : t)));

  const remove = (key: number) => setTasks((ts) => ts.filter((t) => t.key !== key));

  const move = (key: number, dir: -1 | 1) =>
    setTasks((ts) => {
      const i = ts.findIndex((t) => t.key === key);
      const j = i + dir;
      if (i < 0 || j < 0 || j >= ts.length) return ts;
      const next = [...ts];
      [next[i], next[j]] = [next[j], next[i]];
      return next;
    });

  const addTask = () =>
    setTasks((ts) => [...ts, { key: nextKey++, title: "", agent: defaultAgent, slices: "", doneWhen: "" }]);

  const generate = async (force: boolean) => {
    if (submitting) return;
    setSubmitting(true);
    setError(null);
    try {
      const payload = {
        goal: goal.trim(),
        mode,
        tasks: tasks.map<PlanTask>((t) => ({
          title: t.title.trim(),
          agent: t.agent,
          slices: t.slices.split("\n").map((s) => s.trim()).filter(Boolean),
          doneWhen: t.doneWhen.trim(),
        })),
        maxMajorTasks: maxTasks ? parseInt(maxTasks, 10) : null,
        force,
      };
      const res = await submitFlightPlan(payload);
      if (res.success) {
        setResult(res);
        setNeedsForce(null);
        onPlanned();
      } else if (res.needsForce) {
        setNeedsForce(res.error ?? "SUPERVISOR.md already exists — overwrite?");
      } else {
        setError(res.error ?? "plan generation failed");
      }
    } finally {
      setSubmitting(false);
    }
  };

  const copyCommand = async () => {
    if (!result?.launchCommand) return;
    try {
      await navigator.clipboard.writeText(result.launchCommand);
      setCopied(true);
      setTimeout(() => setCopied(false), 1600);
    } catch {
      /* clipboard unavailable — the command is visible + selectable below */
    }
  };

  const canDraft = goal.trim().length > 0;
  const canGenerate =
    drafted && tasks.length > 0 && tasks.every((t) => t.title.trim() && t.agent) && !submitting;

  // ── post-generate: the plan is written; hand over to the terminal ──
  if (result) {
    return (
      <div className="px-4 pb-4 pt-3.5">
        <div
          className="mb-3 rounded-xl px-3.5 py-2.5 text-[13px]"
          style={{ background: "rgba(133,189,147,0.08)", border: "1px solid rgba(133,189,147,0.35)", color: "#a9d3b2" }}
        >
          ✓ Flight plan written to <code className="font-mono text-[12px]">{result.path}</code>
          {result.archivedPrevious && (
            <span className="text-muted"> · previous flight archived as {result.archivedPrevious}</span>
          )}
        </div>

        <div className="mb-1.5 font-mono text-[10.5px] font-bold tracking-[0.12em]" style={{ color: NIGHT }}>
          LAUNCH IT IN YOUR TERMINAL — THE ONE STEP THE WEB CAN'T DO
        </div>
        <div className="flex items-center gap-2">
          <input
            readOnly
            value={result.launchCommand ?? ""}
            onFocus={(e) => e.currentTarget.select()}
            className="min-w-0 flex-1 rounded-xl border border-line-soft bg-field px-3.5 py-2.5 font-mono text-[12.5px] text-ink outline-none"
          />
          <button
            onClick={copyCommand}
            className="flex-shrink-0 rounded-xl px-4 py-2.5 text-[13px] font-bold text-[#171319]"
            style={{ background: NIGHT }}
          >
            {copied ? "Copied ✓" : "Copy"}
          </button>
        </div>
        <p className="mt-2 text-[12px] leading-[1.55] text-faint">
          The flight is an interactive <code className="font-mono">claude --agent supervisor</code> session —
          start it when you're ready (tonight, tmux, anywhere). It reads this plan, flies it, checkpoints to{" "}
          <code className="font-mono">.quorum/autopilot/</code>, and the flights board here shows the morning
          review when you're back.
        </p>

        <details className="mt-3">
          <summary className="cursor-pointer select-none font-mono text-[11px] text-muted hover:text-ink">
            preview SUPERVISOR.md
          </summary>
          <pre className="mt-2 max-h-72 overflow-auto rounded-xl border border-line-soft bg-field px-3.5 py-3 font-mono text-[11px] leading-[1.6] text-[#b9b2ba]">
            {result.content}
          </pre>
        </details>

        <button
          onClick={() => {
            setResult(null);
            setDrafted(false);
            setTasks([]);
            setGoal("");
          }}
          className="mt-3 rounded-full border border-line-soft px-3.5 py-1.5 text-[12px] text-muted hover:border-line-dash hover:text-ink"
        >
          Plan another flight
        </button>
      </div>
    );
  }

  // ── compose: goal → drafted tasks → generate ──
  return (
    <div className="px-4 pb-4 pt-3.5">
      <textarea
        value={goal}
        onChange={(e) => setGoal(e.target.value)}
        placeholder={
          "One high-level goal for the overnight flight — e.g. “Wire the Kafka consumer end-to-end, then cover it with integration tests; draft the decision note.”"
        }
        className="block h-[64px] w-full resize-y rounded-xl border border-line-soft bg-field px-3.5 py-2.5 text-sm leading-[1.5] text-ink outline-none focus:border-[#55506a]"
      />
      <div className="mt-2.5 flex items-center gap-2">
        <button
          onClick={draft}
          disabled={!canDraft}
          className="rounded-xl px-4 py-2 text-[13px] font-bold text-[#171319] transition-opacity disabled:cursor-not-allowed disabled:opacity-45"
          style={{ background: NIGHT }}
        >
          {drafted ? "Re-draft tasks" : "Draft major tasks"}
        </button>
        <span className="text-[11.5px] text-faint">
          splits the goal into editable major tasks — slices fan out in parallel inside each
        </span>
      </div>

      {drafted && (
        <div className="mt-3.5 flex flex-col gap-2">
          <div className="font-mono text-[10.5px] font-bold tracking-[0.12em]" style={{ color: NIGHT }}>
            MAJOR TASKS · SEQUENTIAL · {tasks.length}
          </div>
          {tasks.map((t, i) => (
            <div key={t.key} className="rounded-xl border border-line-soft bg-field px-3 py-2.5">
              <div className="flex items-center gap-2">
                <span className="w-6 flex-shrink-0 font-mono text-[11px] text-faint">{i + 1}.</span>
                <input
                  value={t.title}
                  onChange={(e) => update(t.key, { title: e.target.value })}
                  placeholder="major task title"
                  className="min-w-0 flex-1 rounded-lg border border-transparent bg-transparent px-1.5 py-1 text-[13.5px] font-semibold text-ink outline-none focus:border-line-dash"
                />
                <select
                  value={t.agent}
                  onChange={(e) => update(t.key, { agent: e.target.value })}
                  className="flex-shrink-0 rounded-lg border border-line-soft bg-chip px-2 py-1 font-mono text-[11.5px] text-ink outline-none"
                  title="roster agent that flies this task's slices"
                >
                  {agents.map((a) => (
                    <option key={a.id} value={a.id}>
                      {a.id}
                    </option>
                  ))}
                </select>
                <div className="flex flex-shrink-0 items-center gap-0.5">
                  <button
                    onClick={() => move(t.key, -1)}
                    disabled={i === 0}
                    className="h-6 w-6 rounded-md border border-line-soft text-[11px] text-muted hover:text-ink disabled:opacity-30"
                    title="move up"
                  >
                    ↑
                  </button>
                  <button
                    onClick={() => move(t.key, 1)}
                    disabled={i === tasks.length - 1}
                    className="h-6 w-6 rounded-md border border-line-soft text-[11px] text-muted hover:text-ink disabled:opacity-30"
                    title="move down"
                  >
                    ↓
                  </button>
                  <button
                    onClick={() => remove(t.key)}
                    className="h-6 w-6 rounded-md border border-line-soft text-[11px] text-muted hover:border-[rgba(201,139,129,0.5)] hover:text-[#c98b81]"
                    title="remove task"
                  >
                    ✕
                  </button>
                </div>
              </div>
              <div className="mt-1.5 grid gap-1.5 pl-8" style={{ gridTemplateColumns: "1fr 240px" }}>
                <textarea
                  value={t.slices}
                  onChange={(e) => update(t.key, { slices: e.target.value })}
                  placeholder="parallel slices — one per line (optional; defaults to the title)"
                  className="h-[52px] resize-y rounded-lg border border-line-soft bg-panel px-2.5 py-1.5 text-[12px] leading-[1.5] text-ink outline-none focus:border-line-dash"
                />
                <input
                  value={t.doneWhen}
                  onChange={(e) => update(t.key, { doneWhen: e.target.value })}
                  placeholder="done when… (optional)"
                  className="self-start rounded-lg border border-line-soft bg-panel px-2.5 py-1.5 text-[12px] text-ink outline-none focus:border-line-dash"
                />
              </div>
            </div>
          ))}
          <button
            onClick={addTask}
            className="self-start rounded-full border border-dashed border-line-dash px-3.5 py-1.5 text-[12px] text-muted hover:text-ink"
          >
            + add task
          </button>

          <div className="mt-1 flex items-center gap-3">
            <label className="flex items-center gap-2 text-[12px] text-muted">
              stop after
              <input
                value={maxTasks}
                onChange={(e) => setMaxTasks(e.target.value.replace(/[^\d]/g, ""))}
                placeholder="—"
                className="w-12 rounded-lg border border-line-soft bg-field px-2 py-1 text-center font-mono text-[12px] text-ink outline-none"
              />
              major tasks
            </label>
            <span className="text-[11.5px] text-faint">
              always stops on: context near-full · window exhausted · needs-human
            </span>
          </div>

          {needsForce && (
            <div
              className="mt-1 flex items-center gap-3 rounded-xl px-3.5 py-2.5 text-[12.5px]"
              style={{ background: "rgba(227,164,92,0.08)", border: "1px solid rgba(227,164,92,0.35)", color: "#e8c49a" }}
            >
              <span className="min-w-0 flex-1">{needsForce}</span>
              <button
                onClick={() => generate(true)}
                disabled={submitting}
                className="flex-shrink-0 rounded-lg bg-brand px-3.5 py-1.5 text-[12.5px] font-bold text-[#1a1410] disabled:opacity-45"
              >
                Overwrite
              </button>
              <button
                onClick={() => setNeedsForce(null)}
                className="flex-shrink-0 rounded-lg border border-line-soft px-3 py-1.5 text-[12.5px] text-muted"
              >
                Cancel
              </button>
            </div>
          )}
          {error && (
            <div
              className="mt-1 rounded-xl px-3.5 py-2.5 text-[12.5px]"
              style={{ background: "rgba(201,139,129,0.08)", border: "1px solid rgba(201,139,129,0.35)", color: "#c98b81" }}
            >
              {error}
            </div>
          )}

          <div className="mt-1 flex items-center gap-2.5">
            <button
              onClick={() => generate(false)}
              disabled={!canGenerate}
              className="rounded-xl px-5 py-2.5 text-sm font-bold text-[#171319] transition-opacity disabled:cursor-not-allowed disabled:opacity-45"
              style={{ background: m.color }}
            >
              {submitting ? "Writing…" : "Generate flight plan"}
            </button>
            <span className="text-[11.5px] text-faint">
              writes SUPERVISOR.md + scaffolds the checkpoint — launching stays in your terminal
            </span>
          </div>
        </div>
      )}
    </div>
  );
}
