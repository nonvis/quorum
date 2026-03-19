import { useState, useCallback, useEffect } from "react";
import type { Conversation, Stats, ProjectConfig, ProjectState, Team, Agent } from "./types";
import { fetchConversations, fetchStats, fetchConfig, fetchProjects, fetchTeams, fetchAgents, fetchDaemonStatus } from "./api";
import { useSSE } from "./hooks/useSSE";
import { StatsBanner } from "./components/StatsBanner";
import { ProjectSelector } from "./components/ProjectSelector";
import { TeamSelector } from "./components/TeamSelector";
import { AgentRoster } from "./components/AgentRoster";
import { PromptInput } from "./components/PromptInput";
import { ConversationCard } from "./components/ConversationCard";
import { ConfigPanel } from "./components/ConfigPanel";
import { BudgetPanel } from "./components/BudgetPanel";
import { AgentCreateForm } from "./components/AgentCreateForm";
import { TeamCreateForm } from "./components/TeamCreateForm";
import { AgentContextEditor } from "./components/AgentContextEditor";

export default function App() {
  const [conversations, setConversations] = useState<Conversation[]>([]);
  const [stats, setStats] = useState<Stats | null>(null);
  const [projectConfig, setProjectConfig] = useState<ProjectConfig | null>(null);
  const [showConfig, setShowConfig] = useState(false);
  const [project, setProject] = useState<ProjectState>({ current: null, recent: [] });
  const [teams, setTeams] = useState<Team[]>([]);
  const [agents, setAgents] = useState<Agent[]>([]);
  const [selectedTeam, setSelectedTeam] = useState<string | null>(null);
  const [daemonRunning, setDaemonRunning] = useState(true);
  const [editingAgent, setEditingAgent] = useState<string | null>(null);

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

  // Reload teams, agents, conversations, config when project changes
  useEffect(() => {
    if (project.current) {
      fetchTeams().then((t) => {
        setTeams(t);
        if (t.length > 0 && !selectedTeam) setSelectedTeam(t[0].id);
      });
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

  const handleProjectSelect = async (path: string) => {
    const p = await fetchProjects();
    setProject(p);
  };

  const ACTIVE_STATES = new Set(["active", "waiting_for_human"]);
  const busy = conversations.some((c) => ACTIVE_STATES.has(c.state));

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

      {project.current ? (
        <>
          {teams.length > 0 && (
            <TeamSelector teams={teams} selected={selectedTeam} onSelect={setSelectedTeam} />
          )}
          {project.current && (
            <div className="px-6 py-1">
              <TeamCreateForm
                agents={agents}
                onCreated={() => fetchTeams().then((t) => {
                  setTeams(t);
                  if (t.length > 0 && !selectedTeam) setSelectedTeam(t[0].id);
                })}
              />
            </div>
          )}
          {agents.length > 0 && (
            <AgentRoster
              agents={agents}
              teamPath={teams.find((t) => t.id === selectedTeam)?.default_path ?? []}
              onAgentClick={setEditingAgent}
            />
          )}
          {project.current && (
            <div className="px-6 py-1">
              <AgentCreateForm onCreated={() => fetchAgents().then(setAgents)} />
            </div>
          )}
          <BudgetPanel />
          <PromptInput onSubmit={refresh} busy={busy} team={selectedTeam} />

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
      {showConfig && <ConfigPanel onClose={() => { setShowConfig(false); fetchConfig().then(setProjectConfig); }} />}
    </div>
  );
}
