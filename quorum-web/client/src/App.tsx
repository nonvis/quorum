import { useState, useCallback, useEffect, useRef } from "react";
import type { Conversation, Stats, ProjectConfig, ProjectState, Agent } from "./types";
import { fetchConversations, fetchStats, fetchConfig, fetchProjects, fetchAgents, fetchDaemonStatus } from "./api";
import { useSSE } from "./hooks/useSSE";
import { StatsBanner } from "./components/StatsBanner";
import { ProjectSelector } from "./components/ProjectSelector";
import { AgentRoster } from "./components/AgentRoster";
import { PromptInput } from "./components/PromptInput";
import { ConversationCard } from "./components/ConversationCard";
import { ConfigPanel } from "./components/ConfigPanel";
import { BudgetPanel } from "./components/BudgetPanel";
import { AgentCreateForm } from "./components/AgentCreateForm";
import { AgentContextEditor } from "./components/AgentContextEditor";
import { RecapPanel } from "./components/RecapPanel";

export default function App() {
  const [conversations, setConversations] = useState<Conversation[]>([]);
  const [stats, setStats] = useState<Stats | null>(null);
  const [projectConfig, setProjectConfig] = useState<ProjectConfig | null>(null);
  const [showConfig, setShowConfig] = useState(false);
  const [project, setProject] = useState<ProjectState>({ current: null, recent: [] });
  const [agents, setAgents] = useState<Agent[]>([]);
  const [daemonRunning, setDaemonRunning] = useState(true);
  const [editingAgent, setEditingAgent] = useState<string | null>(null);
  const [showRecap, setShowRecap] = useState(false);

  const refresh = useCallback(async () => {
    const [convs, st, daemon] = await Promise.all([
      fetchConversations(), fetchStats(), fetchDaemonStatus(),
    ]);
    setConversations(convs);
    setStats(st);
    setDaemonRunning(daemon.running);
  }, []);

  // Initial load — always fetch project state; conversations/stats only if project selected
  useEffect(() => {
    fetchProjects().then((p) => {
      setProject(p);
      if (p.current) {
        refresh();
        fetchConfig().then(setProjectConfig);
      }
    });
  }, [refresh]);

  // Reload agents, conversations, config when project changes.
  useEffect(() => {
    if (project.current) {
      fetchAgents().then(setAgents);
      refresh();
      fetchConfig().then(setProjectConfig);
    }
  }, [project.current]);

  // SSE for real-time updates
  useSSE((convs) => {
    setConversations(convs);
    fetchStats().then(setStats);
  });

  // Polling fallback — the SSE stream can stall (dev proxy buffering / dropped
  // socket, and onerror is a no-op), which leaves the UI frozen until a manual
  // refresh. Poll every 2.5s while a conversation is actively running so updates
  // land on their own. A ref holds the latest conversations without re-arming
  // the interval on every update.
  const conversationsRef = useRef(conversations);
  conversationsRef.current = conversations;
  useEffect(() => {
    if (!project.current) return;
    const id = setInterval(() => {
      if (conversationsRef.current.some((c) => c.state === "active")) refresh();
    }, 2500);
    return () => clearInterval(id);
  }, [project.current, refresh]);

  const handleProjectSelect = async (_path: string) => {
    const p = await fetchProjects();
    setProject(p);
  };

  const ACTIVE_STATES = new Set(["active", "waiting_for_human"]);
  const busy = conversations.some((c) => ACTIVE_STATES.has(c.state));
  const runningConvs = conversations.filter((c) => c.state === "active");

  return (
    <div className="min-h-screen bg-zinc-950 text-white">
      <StatsBanner
        stats={stats}
        config={projectConfig}
        onSettingsClick={() => setShowConfig(true)}
        projectName={project.current ? project.current.split("/").pop() : null}
      />
      <ProjectSelector
        current={project.current}
        recent={project.recent}
        onSelect={handleProjectSelect}
      />

      {project.current && !daemonRunning && conversations.some(c =>
        c.state === "active" || c.state === "waiting_for_human"
      ) && (
        <div className="mx-6 mt-3 px-4 py-2 bg-amber-900/30 border border-amber-800 rounded-lg text-amber-400 text-sm">
          Daemon not running — active conversations may be stale.
          Run <code className="bg-zinc-800 px-1.5 py-0.5 rounded text-xs">quorum status</code> to trigger recovery.
        </div>
      )}

      {project.current && runningConvs.length > 0 && (
        <div className="mx-6 mt-3 px-4 py-2 bg-blue-950/30 border border-blue-800 rounded-lg text-sm flex items-center gap-2">
          <span className="relative flex h-2 w-2">
            <span className="animate-ping absolute inline-flex h-full w-full rounded-full bg-blue-400 opacity-75"></span>
            <span className="relative inline-flex rounded-full h-2 w-2 bg-blue-500"></span>
          </span>
          <span className="text-blue-300">
            Running — {runningConvs.map((c) => `${c.current_agent ?? "?"} on #${c.id}`).join(", ")}
          </span>
        </div>
      )}

      {project.current ? (
        <>
          {agents.length > 0 && (
            <AgentRoster
              agents={agents}
              onAgentClick={setEditingAgent}
            />
          )}
          {project.current && (
            <div className="px-6 py-1">
              <AgentCreateForm onCreated={() => fetchAgents().then(setAgents)} />
            </div>
          )}
          <div className="px-6 py-1">
            <button
              onClick={() => setShowRecap(true)}
              className="px-4 py-2 text-sm bg-zinc-800 text-zinc-200 rounded-lg hover:bg-zinc-700 border border-zinc-700"
            >
              What's going on?
            </button>
          </div>
          <BudgetPanel />
          <PromptInput onSubmit={refresh} busy={busy} />

          <div className="px-6 pb-6">
            <h2 className="text-sm font-medium text-zinc-500 uppercase tracking-wide mb-3">
              Conversations
            </h2>
            <div className="space-y-3">
              {conversations.map((conv) => (
                <ConversationCard key={conv.id} conversation={conv} onAction={refresh} />
              ))}
              {conversations.length === 0 && (
                <p className="text-zinc-600 text-sm">
                  No conversations yet. Type a goal above to start.
                </p>
              )}
            </div>
          </div>
        </>
      ) : (
        <div className="px-6 py-16 text-center text-zinc-600">
          <p className="text-lg mb-2">No project selected</p>
          <p className="text-sm">Enter a project path above to get started.</p>
        </div>
      )}
      {editingAgent && (
        <AgentContextEditor
          agentId={editingAgent}
          agentName={agents.find(a => a.id === editingAgent)?.name ?? editingAgent}
          onClose={() => setEditingAgent(null)}
        />
      )}
      {showRecap && <RecapPanel onClose={() => setShowRecap(false)} />}
      {showConfig && <ConfigPanel onClose={() => { setShowConfig(false); fetchConfig().then(setProjectConfig); }} />}
    </div>
  );
}
