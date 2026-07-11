import { useState, useCallback, useEffect, useRef } from "react";
import type { Conversation, ProjectState, Agent } from "./types";
import { fetchConversations, fetchProjects, fetchAgents, fetchDaemonStatus } from "./api";
import { useSSE } from "./hooks/useSSE";
import { TopBar } from "./components/TopBar";
import { Sidebar } from "./components/Sidebar";
import { Composer } from "./components/Composer";
import { ConversationCard } from "./components/ConversationCard";
import { ConversationDetail } from "./components/ConversationDetail";
import { ConfigPanel } from "./components/ConfigPanel";
import { BudgetSheet } from "./components/BudgetSheet";
import { RecapPanel } from "./components/RecapPanel";
import { AgentContextEditor } from "./components/AgentContextEditor";

type Filter = "all" | "needs" | "running" | "done";

const STATE_ORDER: Record<string, number> = {
  waiting_for_human: 0,
  active: 1,
  paused: 2,
  done: 3,
  closed: 4,
};

const FILTER_FNS: Record<Filter, (c: Conversation) => boolean> = {
  all: () => true,
  needs: (c) => c.state === "waiting_for_human",
  running: (c) => c.state === "active",
  done: (c) => c.state === "done" || c.state === "closed",
};

export default function App() {
  const [conversations, setConversations] = useState<Conversation[]>([]);
  const [project, setProject] = useState<ProjectState>({ current: null, recent: [] });
  const [agents, setAgents] = useState<Agent[]>([]);
  const [daemonRunning, setDaemonRunning] = useState(true);
  const [filter, setFilter] = useState<Filter>("all");
  const [selected, setSelected] = useState<{ id: number; respond?: boolean } | null>(null);
  const [editingAgent, setEditingAgent] = useState<string | null>(null);
  const [showConfig, setShowConfig] = useState(false);
  const [showBudget, setShowBudget] = useState(false);
  const [showRecap, setShowRecap] = useState(false);

  const refresh = useCallback(async () => {
    const [convs, daemon] = await Promise.all([fetchConversations(), fetchDaemonStatus()]);
    setConversations(convs);
    setDaemonRunning(daemon.running);
  }, []);

  useEffect(() => {
    fetchProjects().then((p) => {
      setProject(p);
      if (p.current) refresh();
    });
  }, [refresh]);

  useEffect(() => {
    if (!project.current) return;
    fetchAgents().then(setAgents);
    refresh();
  }, [project.current, refresh]);

  useSSE((convs) => setConversations(convs));

  // Polling fallback while a conversation is actively running (SSE can stall).
  const convRef = useRef(conversations);
  convRef.current = conversations;
  useEffect(() => {
    if (!project.current) return;
    const id = setInterval(() => {
      if (convRef.current.some((c) => c.state === "active")) refresh();
    }, 2500);
    return () => clearInterval(id);
  }, [project.current, refresh]);

  const onProjectSelect = async () => {
    const p = await fetchProjects();
    setProject(p);
  };

  const busy = conversations.some((c) => c.state === "active" || c.state === "waiting_for_human");
  const needsCount = conversations.filter((c) => c.state === "waiting_for_human").length;
  const runningCount = conversations.filter((c) => c.state === "active").length;
  const doneCount = conversations.filter((c) => c.state === "done" || c.state === "closed").length;
  const daemonStale = !daemonRunning && (needsCount > 0 || runningCount > 0);

  const sorted = [...conversations].sort(
    (a, b) => (STATE_ORDER[a.state] ?? 9) - (STATE_ORDER[b.state] ?? 9) || b.id - a.id,
  );
  const visible = sorted.filter(FILTER_FNS[filter]);

  const filterDefs: { id: Filter; label: string; count: number }[] = [
    { id: "all", label: "all", count: conversations.length },
    { id: "needs", label: "needs you", count: needsCount },
    { id: "running", label: "running", count: runningCount },
    { id: "done", label: "done", count: doneCount },
  ];

  return (
    <div className="flex min-h-screen flex-col bg-base text-ink">
      <TopBar
        projectCurrent={project.current}
        projectRecent={project.recent}
        onProjectSelect={onProjectSelect}
        daemonRunning={daemonRunning}
        needsCount={needsCount}
        convCount={conversations.length}
        onSettingsClick={() => setShowConfig(true)}
      />

      {project.current ? (
        <div className="grid flex-1" style={{ gridTemplateColumns: "252px minmax(0,1fr)" }}>
          <Sidebar
            agents={agents}
            onAgentClick={setEditingAgent}
            onAgentCreated={() => fetchAgents().then(setAgents)}
            onOpenBudget={() => setShowBudget(true)}
          />

          <main className="min-w-0 px-8 pb-14 pt-6">
            <div className="mx-auto flex w-full max-w-[940px] flex-col">
              {daemonStale && (
                <div
                  className="mb-3 rounded-xl px-3.5 py-2 text-[13px] text-brand"
                  style={{ background: "rgba(227,164,92,0.08)", border: "1px solid rgba(227,164,92,0.3)" }}
                >
                  Daemon idle — active conversations may be stale. Run{" "}
                  <code className="rounded bg-chip px-1.5 py-0.5 font-mono text-xs">quorum status</code> to recover.
                </div>
              )}

              <Composer onSubmit={refresh} busy={busy} />

              <div className="mb-3.5 mt-[22px] flex items-center gap-2">
                <span className="font-mono text-[10.5px] font-semibold tracking-[0.14em] text-faint">
                  CONVERSATIONS
                </span>
                <button
                  onClick={() => setShowRecap(true)}
                  className="rounded-full border border-line-soft bg-transparent px-3 py-1 text-[11.5px] text-muted hover:border-line-dash hover:text-ink"
                  title="On-demand recap (quorum ask --agent recap)"
                >
                  what's going on?
                </button>
                <span className="flex-1" />
                {filterDefs.map((f) => {
                  const active = filter === f.id;
                  const highlight = f.id === "needs" && f.count > 0;
                  return (
                    <button
                      key={f.id}
                      onClick={() => setFilter(f.id)}
                      className="rounded-full px-3 py-1.5 font-mono text-[11.5px] tracking-[0.03em] transition-colors"
                      style={{
                        background: active ? "#2b2735" : "transparent",
                        color: highlight ? "#e3a45c" : active ? "#ece7e1" : "#8a8390",
                        border: `1px solid ${active ? "#3b3546" : "#2a2632"}`,
                      }}
                    >
                      {f.label} · {f.count}
                    </button>
                  );
                })}
              </div>

              <div className="flex flex-col gap-3">
                {visible.map((c) => (
                  <ConversationCard
                    key={c.id}
                    conversation={c}
                    onOpen={(id, opts) => setSelected({ id, respond: opts?.respond })}
                    onAction={refresh}
                  />
                ))}
                {visible.length === 0 && (
                  <p className="mx-1 my-2 text-[13px] text-faint">
                    {conversations.length === 0
                      ? "No conversations yet. Set a mode and a goal above to start."
                      : "Nothing here right now."}
                  </p>
                )}
              </div>
            </div>
          </main>
        </div>
      ) : (
        <div className="flex flex-1 flex-col items-center justify-center px-6 py-16 text-center">
          <p className="mb-2 text-lg text-muted">No project selected</p>
          <p className="text-sm text-faint">
            Use the project menu in the top bar to open a directory with a{" "}
            <code className="rounded bg-chip px-1.5 py-0.5 font-mono text-xs">.quorum/</code>.
          </p>
        </div>
      )}

      {selected && (
        <ConversationDetail
          conversationId={selected.id}
          initialRespond={selected.respond}
          agents={agents}
          onClose={() => setSelected(null)}
          onAction={refresh}
        />
      )}
      {editingAgent && (
        <AgentContextEditor
          agentId={editingAgent}
          agentName={agents.find((a) => a.id === editingAgent)?.name ?? editingAgent}
          onClose={() => setEditingAgent(null)}
        />
      )}
      {showBudget && <BudgetSheet onClose={() => setShowBudget(false)} />}
      {showRecap && <RecapPanel onClose={() => setShowRecap(false)} />}
      {showConfig && <ConfigPanel onClose={() => setShowConfig(false)} />}
    </div>
  );
}
