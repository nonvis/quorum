import { useState, useEffect, useRef, useMemo } from "react";
import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";
import type { Conversation, Task, Agent, PendingVaultUpdate } from "../types";
import { fetchConversation, fetchPendingVault, respondToLeader, updateMaxRounds } from "../api";
import { parseSegments, extractHumanResponse, lastHumanGateMessage } from "../lib/segments";
import { deriveVerdict, VERDICT_COLOR } from "../lib/verdict";
import { DiffBlock } from "./DiffBlock";
import { GateChips, VaultManifest } from "./GateActions";
import {
  modeOf,
  stateOf,
  roleColor,
  ktok,
  fmtUsd,
  fmtElapsedSince,
} from "../lib/theme";

function TaskBody({ task }: { task: Task }) {
  const segments = task.result ? parseSegments(task.result) : [];
  const you = extractHumanResponse(task.prompt);
  return (
    <div className="mt-2 flex flex-col gap-2.5">
      {you && (
        <div
          className="flex items-baseline gap-2.5 rounded-xl px-3 py-2.5"
          style={{ background: "rgba(227,164,92,0.08)", border: "1px solid rgba(227,164,92,0.3)" }}
        >
          <span className="flex-shrink-0 font-mono text-[10px] font-bold tracking-[0.1em] text-brand">YOU</span>
          <span className="whitespace-pre-wrap text-[13px] leading-[1.55] text-ink">{you}</span>
        </div>
      )}
      {segments.map((seg, i) =>
        seg.kind === "prose" ? (
          <div key={i} className="md-content">
            <ReactMarkdown remarkPlugins={[remarkGfm]}>{seg.text}</ReactMarkdown>
          </div>
        ) : seg.kind === "handoff" ? (
          <div key={i} className="overflow-hidden rounded-xl border border-line-soft">
            <div className="flex items-center gap-2 bg-chip px-3 py-1.5 font-mono text-[10.5px] tracking-[0.06em]">
              <span className="text-faint">→ HANDOFF</span>
              <span className="font-semibold text-running">{seg.to}</span>
            </div>
            <div className="md-content bg-rail px-3 py-2.5">
              <ReactMarkdown remarkPlugins={[remarkGfm]}>{seg.prompt}</ReactMarkdown>
            </div>
          </div>
        ) : (
          // A vault write is a CHANGE — render it as a diff, not prose.
          <div key={i} className="overflow-hidden rounded-xl" style={{ border: "1px solid rgba(167,147,230,0.3)" }}>
            <div
              className="flex items-center gap-2 px-3 py-1.5 font-mono text-[10.5px] tracking-[0.06em]"
              style={{ background: "rgba(167,147,230,0.09)" }}
            >
              <span style={{ color: "#a793e6" }}>▤ VAULT WRITE</span>
              <span className="break-all" style={{ color: "#c8bbef" }}>{seg.path}</span>
            </div>
            <DiffBlock text={seg.content} />
          </div>
        )
      )}
      {task.error && (
        <div
          className="whitespace-pre-wrap rounded-xl px-3 py-2.5 font-mono text-[12px] leading-[1.6]"
          style={{ background: "rgba(201,139,129,0.08)", border: "1px solid rgba(201,139,129,0.35)", color: "#c98b81" }}
        >
          {task.error}
        </div>
      )}
    </div>
  );
}

// The canonical verdict-first task unit: status dot + agent + role + a
// one-line VERDICT always visible; full prose collapsed by default —
// not-expanding is the happy path.
function TaskRow({
  task,
  isLast,
  role,
  now,
}: {
  task: Task;
  isLast: boolean;
  role: string;
  now: number;
}) {
  const [expanded, setExpanded] = useState(false);
  const working = task.status === "active";
  const dotBg =
    task.status === "done" ? "#85bd93" : working ? "#8fa9e8" : task.status === "failed" ? "#c98b81" : "#4a4454";
  const hasBody = !!task.result || !!task.error || !!extractHumanResponse(task.prompt);
  const liveCost = working ? "" : task.cost != null ? fmtUsd(task.cost) : "";
  const verdict = deriveVerdict(task);

  return (
    <div className="relative mt-4 pl-6">
      {!isLast && <span className="absolute left-[5px] top-6 -bottom-4 w-px bg-line-soft" />}
      <span
        className="absolute left-0 top-[6px] h-[11px] w-[11px] rounded-full"
        style={{ background: dotBg }}
      />
      <div
        onClick={() => hasBody && setExpanded((v) => !v)}
        className={`-mx-1 rounded-md px-1 py-0.5 ${hasBody ? "cursor-pointer select-none hover:bg-chip" : ""}`}
      >
        <div className="flex items-center gap-2.5">
          <span className="w-2.5 text-[10px] text-faint">{hasBody ? (expanded ? "▾" : "▸") : ""}</span>
          <span className="font-mono text-[13px] font-semibold text-ink">{task.agent}</span>
          <span
            className="rounded-full border border-line-soft px-[7px] py-px text-[10.5px]"
            style={{ color: roleColor(role) }}
          >
            {role}
          </span>
          <span className="flex-1" />
          {task.token_in != null && (
            <span className="font-mono text-[11px] text-faint">
              {ktok(task.token_in)} → {task.token_out != null ? ktok(task.token_out) : "…"}
            </span>
          )}
          {liveCost && <span className="font-mono text-[11.5px] text-muted">{liveCost}</span>}
        </div>
        {verdict.kind !== "none" && (
          <div className="mt-1 flex items-baseline gap-2 pl-[22px]">
            <span
              className="flex-shrink-0 font-mono text-[10px] font-bold tracking-[0.08em]"
              style={{ color: VERDICT_COLOR[verdict.kind] }}
            >
              {verdict.kind === "error" ? "✕" : verdict.kind === "handoff" ? "→" : verdict.kind === "vault" ? "▤" : "▸"}
            </span>
            <span
              className="line-clamp-1 text-[12.5px] leading-[1.5]"
              style={{ color: verdict.kind === "error" ? "#c98b81" : "#b9b2ba" }}
              title={verdict.text}
            >
              {verdict.text}
            </span>
          </div>
        )}
      </div>

      {working && (
        <div
          className="mt-2 flex items-center gap-2.5 rounded-xl px-3 py-2.5"
          style={{ background: "rgba(143,169,232,0.07)", border: "1px solid rgba(143,169,232,0.3)" }}
        >
          <span className="q-pulse font-mono text-[12px] text-running">◉ working</span>
          <span className="font-mono text-[12px] text-muted">
            {fmtElapsedSince(task.started_at ?? task.created_at, now)}
          </span>
          <span className="q-bar h-[3px] flex-1 rounded-full" />
        </div>
      )}

      {expanded && hasBody && <TaskBody task={task} />}
    </div>
  );
}

export function ConversationDetail({
  conversationId,
  initialRespond,
  agents,
  onClose,
  onAction,
}: {
  conversationId: number;
  initialRespond?: boolean;
  agents: Agent[];
  onClose: () => void;
  onAction: () => void;
}) {
  const [conv, setConv] = useState<Conversation | null>(null);
  const [tasks, setTasks] = useState<Task[]>([]);
  const [pendingVault, setPendingVault] = useState<PendingVaultUpdate[]>([]);
  const [now, setNow] = useState(() => Date.now());
  const [respondText, setRespondText] = useState("");
  const [sending, setSending] = useState(false);
  const [maxRoundsInput, setMaxRoundsInput] = useState("");
  const respondRef = useRef<HTMLTextAreaElement>(null);

  const roleFor = useMemo(() => {
    // Tasks reference agents by id ("leader") while display names may differ
    // ("Leader") — index both, case-insensitively.
    const map = new Map<string, string>();
    for (const a of agents) {
      map.set(a.id.toLowerCase(), a.role);
      map.set(a.name.toLowerCase(), a.role);
    }
    return (name: string) => map.get(name.toLowerCase()) ?? "doer";
  }, [agents]);

  const load = () =>
    fetchConversation(conversationId).then((data) => {
      setConv(data);
      setTasks(data.tasks ?? []);
      // The approval manifest only exists while a brainstorm holds the gate.
      if (data.state === "waiting_for_human" && data.mode === "brainstorm") {
        fetchPendingVault(conversationId).then(setPendingVault);
      } else {
        setPendingVault([]);
      }
    });

  useEffect(() => {
    load();
  }, [conversationId]);

  useEffect(() => {
    if (conv?.state !== "active") return;
    const id = setInterval(load, 2000);
    return () => clearInterval(id);
  }, [conversationId, conv?.state]);

  useEffect(() => {
    const active = tasks.some((t) => t.status === "active");
    if (!active) return;
    setNow(Date.now());
    const id = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(id);
  }, [tasks]);

  useEffect(() => {
    if (initialRespond) respondRef.current?.focus();
  }, [initialRespond, conv?.state]);

  // Send a response — canned chip text or the textarea's contents.
  const send = async (canned?: string) => {
    const text = (canned ?? respondText).trim();
    if (!text || sending) return;
    setSending(true);
    try {
      await respondToLeader(conversationId, text);
      setRespondText("");
      await load();
      onAction();
    } finally {
      setSending(false);
    }
  };

  const focusDiscuss = () => {
    setRespondText((t) => t || "Not yet — ");
    respondRef.current?.focus();
  };

  const resume = async (rounds: number) => {
    if (!conv) return;
    await updateMaxRounds(conversationId, rounds);
    setMaxRoundsInput("");
    await load();
    onAction();
  };

  const m = modeOf(conv?.mode);
  const st = conv ? stateOf(conv.state) : null;
  const n = tasks.length;
  const waiting = conv?.state === "waiting_for_human";
  const paused = conv?.state === "paused";
  const gateText = waiting ? lastHumanGateMessage(tasks) ?? conv?.paused_reason ?? "" : "";
  const roundPct = conv ? Math.min(100, (conv.round / Math.max(1, conv.max_rounds)) * 100) : 0;
  const nearCap = conv ? conv.round / Math.max(1, conv.max_rounds) >= 0.85 : false;

  return (
    <div
      className="q-fade fixed inset-0 z-[60] flex justify-end"
      style={{ background: "rgba(11,9,15,0.55)", backdropFilter: "blur(3px)" }}
      onClick={onClose}
    >
      <div
        onClick={(e) => e.stopPropagation()}
        className="q-slide flex h-full w-[820px] max-w-[96vw] flex-col bg-sheet"
        style={{ borderLeft: "1px solid #322d3c", boxShadow: "-24px 0 64px rgba(0,0,0,0.45)" }}
      >
        {/* mode band header */}
        <div
          className="flex flex-shrink-0 items-center gap-2.5 border-b border-line px-5 py-2.5"
          style={{ background: m.bandBg }}
        >
          <span className="font-mono text-[11px] font-bold tracking-[0.09em]" style={{ color: m.color }}>
            {m.banner}
          </span>
          <span className="flex-1" />
          <button
            onClick={onClose}
            className="h-7 w-7 rounded-lg border border-line-edge text-[13px] text-muted hover:border-line-dash hover:text-ink"
          >
            ✕
          </button>
        </div>

        {conv && (
          <>
            {/* meta + goal */}
            <div className="flex-shrink-0 border-b border-line px-6 pb-4 pt-[18px]">
              <div className="mx-auto flex max-w-[720px] items-center gap-2.5">
                <span className="font-mono text-xs text-faint">#{conv.id}</span>
                {st && (
                  <span
                    className="rounded-full px-2.5 py-[3px] font-mono text-[10.5px] font-bold uppercase tracking-[0.06em]"
                    style={{ background: st.bg, color: st.color }}
                  >
                    {st.label}
                  </span>
                )}
                <span
                  className="inline-flex items-center gap-1.5 rounded-full px-2.5 py-[2px] font-mono text-[10.5px]"
                  style={{ border: `1px solid ${m.chipBorder}`, color: m.color, background: m.chipBg }}
                >
                  {m.icon} {m.label}
                </span>
                <span className="flex-1" />
                <span className="font-mono text-[12px] text-muted" title="spent this conversation">
                  {fmtUsd(conv.spent_usd)}
                </span>
                <span
                  className="font-mono text-[11px]"
                  style={{ color: nearCap ? "#e3a45c" : "#6a6470" }}
                  title="round cap is the real per-conversation limiter"
                >
                  round {conv.round}/{conv.max_rounds}
                </span>
                <span className="inline-block h-[5px] w-[90px] overflow-hidden rounded-full bg-line">
                  <span
                    className="block h-full rounded-full transition-[width] duration-1000"
                    style={{ width: `${roundPct}%`, background: nearCap ? "#e3a45c" : "#63b3a6" }}
                  />
                </span>
              </div>
              <h2 className="mx-auto mt-2.5 max-w-[720px] text-[19px] font-bold leading-[1.4] text-ink-bright [text-wrap:pretty]">
                {conv.goal}
              </h2>
            </div>

            {/* task timeline */}
            <div className="flex-1 overflow-y-auto px-6 pb-6 pt-2.5">
              <div className="mx-auto max-w-[720px]">
                {tasks.length === 0 && (
                  <p className="mt-6 text-[13px] text-faint">No tasks yet.</p>
                )}
                {tasks.map((t, i) => (
                  <TaskRow
                    key={t.id}
                    task={t}
                    isLast={i === n - 1}
                    role={roleFor(t.agent)}
                    now={now}
                  />
                ))}
              </div>
            </div>

            {/* respond footer — canned actions first, free text second */}
            {waiting && (
              <div
                className="max-h-[46vh] flex-shrink-0 overflow-y-auto px-6 pb-[18px] pt-[15px]"
                style={{ borderTop: "1px solid rgba(227,164,92,0.3)", background: "rgba(227,164,92,0.05)" }}
              >
                <div className="mx-auto mb-[7px] max-w-[720px] font-mono text-[10px] font-bold tracking-[0.14em] text-brand">
                  ◆ {(conv.current_agent ?? "LEADER").toUpperCase()} IS WAITING ON YOU
                </div>
                {gateText && (
                  <div className="mx-auto max-h-[110px] max-w-[720px] overflow-y-auto whitespace-pre-wrap text-[13px] leading-[1.55] text-[#d8d2ca]">
                    {gateText}
                  </div>
                )}
                <div className="mx-auto mt-3 max-w-[720px]">
                  {conv.mode === "brainstorm" && pendingVault.length > 0 ? (
                    <VaultManifest
                      rows={pendingVault}
                      onSend={(text) => send(text)}
                      onDiscuss={focusDiscuss}
                      disabled={sending}
                    />
                  ) : (
                    <GateChips onSend={(text) => send(text)} disabled={sending} />
                  )}
                </div>
                <textarea
                  ref={respondRef}
                  value={respondText}
                  onChange={(e) => setRespondText(e.target.value)}
                  onKeyDown={(e) => {
                    if (e.key === "Enter" && (e.metaKey || e.ctrlKey)) send();
                  }}
                  placeholder="Or type your answer — ⌘⏎ to send"
                  className="mx-auto mt-3 block h-[72px] w-full max-w-[720px] resize-y rounded-xl border border-line-dash bg-field px-3 py-2.5 text-[13.5px] leading-[1.5] text-ink outline-none focus:border-[rgba(227,164,92,0.6)]"
                />
                <div className="mx-auto mt-2.5 flex max-w-[720px] justify-end">
                  <button
                    onClick={() => send()}
                    disabled={!respondText.trim() || sending}
                    className="rounded-lg bg-brand px-[18px] py-2.5 text-[13.5px] font-bold text-[#1a1410] hover:bg-brand-bright disabled:opacity-45"
                  >
                    {sending ? "Sending…" : `Send to ${conv.current_agent ?? "leader"}`}
                  </button>
                </div>
              </div>
            )}

            {/* paused footer — raise the round cap (the real limiter) and resume */}
            {paused && (
              <div className="flex flex-shrink-0 items-center gap-3 border-t border-line-edge px-6 py-3.5">
                <span className="text-[12.5px] text-[#b3aa98]">
                  ⏸ Paused{conv.paused_reason ? ` — ${conv.paused_reason}` : ""}
                </span>
                <span className="flex-1" />
                <input
                  type="number"
                  min={conv.max_rounds + 1}
                  step={5}
                  value={maxRoundsInput}
                  onChange={(e) => setMaxRoundsInput(e.target.value)}
                  placeholder={String(conv.max_rounds)}
                  className="w-20 rounded-lg border border-line-dash bg-field px-2 py-1.5 font-mono text-xs text-ink outline-none focus:border-faint"
                />
                <button
                  onClick={() => {
                    const v = parseInt(maxRoundsInput, 10);
                    resume(Number.isFinite(v) && v > conv.max_rounds ? v : conv.max_rounds + 10);
                  }}
                  className="rounded-lg border border-line-dash bg-transparent px-3.5 py-1.5 text-[12.5px] font-semibold text-[#c9c3bd] hover:border-faint hover:text-ink"
                >
                  {maxRoundsInput ? "Set & Resume" : "Resume +10 rounds"}
                </button>
              </div>
            )}
          </>
        )}
      </div>
    </div>
  );
}
