import { useState, useCallback, useEffect, useRef } from "react";
import type { Conversation, ProjectState, Agent, Flight } from "./types";
import {
  fetchConversations,
  fetchProjects,
  fetchAgents,
  fetchDaemonStatus,
  fetchFlights,
  setFlightsReviewed,
} from "./api";
import { useSSE } from "./hooks/useSSE";
import { TopBar } from "./components/TopBar";
import { Sidebar } from "./components/Sidebar";
import { Composer } from "./components/Composer";
import { ConversationCard } from "./components/ConversationCard";
import { ConversationDetail } from "./components/ConversationDetail";
import { FlightCard } from "./components/FlightCard";
import { FlightDetail } from "./components/FlightDetail";
import { ConfigPanel } from "./components/ConfigPanel";
import { BudgetSheet } from "./components/BudgetSheet";
import { RecapPanel } from "./components/RecapPanel";
import { AgentContextEditor } from "./components/AgentContextEditor";

type Filter = "all" | "needs" | "running" | "done" | "flights";

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
  flights: () => false, // flights filter shows the flights band only
};

const FLIGHT_ORDER: Record<string, number> = {
  needs_you: 0,
  in_flight: 1,
  ready: 2,
  complete: 3,
};

// Which flights show under each filter. The board is triage-first: "all"
// hides cleared flights; "flights" is the engine view showing everything
// (cleared ones dimmed).
function visibleFlightsFor(filter: Filter, flights: Flight[]): Flight[] {
  const sorted = [...flights].sort(
    (a, b) =>
      (FLIGHT_ORDER[a.status] ?? 9) - (FLIGHT_ORDER[b.status] ?? 9) ||
      (b.updatedAt ?? "").localeCompare(a.updatedAt ?? ""),
  );
  switch (filter) {
    case "all":
      return sorted.filter((f) => !f.reviewed);
    case "flights":
      return sorted;
    case "needs":
      return sorted.filter((f) => f.status === "needs_you" && !f.reviewed);
    case "running":
      return sorted.filter((f) => f.status === "in_flight" && !f.reviewed);
    case "done":
      return sorted.filter((f) => f.status === "complete");
  }
}

export default function App() {
  const [conversations, setConversations] = useState<Conversation[]>([]);
  const [flights, setFlights] = useState<Flight[]>([]);
  const [project, setProject] = useState<ProjectState>({ current: null, recent: [] });
  const [agents, setAgents] = useState<Agent[]>([]);
  const [daemonRunning, setDaemonRunning] = useState(true);
  const [filter, setFilter] = useState<Filter>("all");
  const [selected, setSelected] = useState<{ id: number; respond?: boolean } | null>(null);
  const [selectedFlight, setSelectedFlight] = useState<string | null>(null);
  const [editingAgent, setEditingAgent] = useState<string | null>(null);
  const [showConfig, setShowConfig] = useState(false);
  const [showBudget, setShowBudget] = useState(false);
  const [showRecap, setShowRecap] = useState(false);

  const refresh = useCallback(async () => {
    const [convs, daemon, fls] = await Promise.all([
      fetchConversations(),
      fetchDaemonStatus(),
      fetchFlights(),
    ]);
    setConversations(convs);
    setDaemonRunning(daemon.running);
    setFlights(fls);
  }, []);

  useEffect(() => {
    fetchProjects().then((p) => {
      setProject(p);
      if (p.current) refresh();
    });
  }, [refresh]);

  // Deep links: #c<id>[/respond] opens a conversation; #f:<id> opens a flight.
  useEffect(() => {
    const m = location.hash.match(/^#c(\d+)(\/respond)?$/);
    if (m) setSelected({ id: Number(m[1]), respond: !!m[2] });
    const fm = location.hash.match(/^#f:(.+)$/);
    if (fm) setSelectedFlight(decodeURIComponent(fm[1]));
  }, []);

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
  const flightNeeds = flights.filter((f) => f.status === "needs_you" && !f.reviewed).length;
  const needsCount = conversations.filter((c) => c.state === "waiting_for_human").length + flightNeeds;
  const runningCount = conversations.filter((c) => c.state === "active").length;
  const doneCount = conversations.filter((c) => c.state === "done" || c.state === "closed").length;
  const daemonStale = !daemonRunning && (needsCount - flightNeeds > 0 || runningCount > 0);

  const sorted = [...conversations].sort(
    (a, b) => (STATE_ORDER[a.state] ?? 9) - (STATE_ORDER[b.state] ?? 9) || b.id - a.id,
  );
  const visible = filter === "flights" ? [] : sorted.filter(FILTER_FNS[filter]);
  const visibleFlights = visibleFlightsFor(filter, flights);
  // Batch-clear targets: landed flights still on the board.
  const clearable = visibleFlights.filter((f) => f.status === "complete" && !f.reviewed);

  const clearAll = async () => {
    if (clearable.length === 0) return;
    await setFlightsReviewed(clearable.map((f) => f.id), true);
    refresh();
  };

  const filterDefs: { id: Filter; label: string; count: number }[] = [
    { id: "all", label: "all", count: conversations.length + flights.filter((f) => !f.reviewed).length },
    { id: "needs", label: "needs you", count: needsCount },
    { id: "running", label: "running", count: runningCount },
    { id: "done", label: "done", count: doneCount },
    { id: "flights", label: "✈ flights", count: flights.filter((f) => !f.reviewed).length },
  ];

  const openFlight = selectedFlight ? flights.find((f) => f.id === selectedFlight) : null;

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

              <Composer onSubmit={refresh} busy={busy} agents={agents} />

              {/* flights band — the morning review: glance → verify → clear */}
              {visibleFlights.length > 0 && (
                <>
                  <div className="mb-3 mt-[22px] flex items-center gap-2">
                    <span className="font-mono text-[10.5px] font-semibold tracking-[0.14em] text-faint">
                      FLIGHTS · MORNING REVIEW
                    </span>
                    <span className="flex-1" />
                    {clearable.length >= 2 && (
                      <button
                        onClick={clearAll}
                        className="rounded-full border px-3 py-1 text-[11.5px] font-semibold transition-colors"
                        style={{
                          borderColor: "rgba(133,189,147,0.4)",
                          color: "#85bd93",
                          background: "rgba(133,189,147,0.06)",
                        }}
                        title="mark every landed flight as reviewed"
                      >
                        ✓ Clear all reviewed · {clearable.length}
                      </button>
                    )}
                  </div>
                  <div className="flex flex-col gap-3">
                    {visibleFlights.map((f) => (
                      <FlightCard key={f.id} flight={f} onOpen={setSelectedFlight} onAction={refresh} />
                    ))}
                  </div>
                </>
              )}
              {filter === "flights" && visibleFlights.length === 0 && (
                <p className="mx-1 mt-5 text-[13px] text-faint">
                  No flights yet — switch the composer to “✈ queue a flight” to plan one.
                </p>
              )}

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
                {visible.length === 0 && filter !== "flights" && (
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
      {openFlight && (
        <FlightDetail
          flight={openFlight}
          onClose={() => setSelectedFlight(null)}
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
