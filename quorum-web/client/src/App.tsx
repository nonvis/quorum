import { useState, useCallback, useEffect } from "react";
import type { Conversation, Stats, ProjectConfig } from "./types";
import { fetchConversations, fetchStats, fetchConfig } from "./api";
import { useSSE } from "./hooks/useSSE";
import { StatsBanner } from "./components/StatsBanner";
import { PromptInput } from "./components/PromptInput";
import { ConversationCard } from "./components/ConversationCard";

export default function App() {
  const [conversations, setConversations] = useState<Conversation[]>([]);
  const [stats, setStats] = useState<Stats | null>(null);
  const [projectConfig, setProjectConfig] = useState<ProjectConfig | null>(null);

  const refresh = useCallback(async () => {
    const [convs, st] = await Promise.all([fetchConversations(), fetchStats()]);
    setConversations(convs);
    setStats(st);
  }, []);

  // Initial load
  useEffect(() => {
    refresh();
    fetchConfig().then(setProjectConfig);
  }, [refresh]);

  // SSE for real-time updates
  useSSE((convs) => {
    setConversations(convs);
    fetchStats().then(setStats);
  });

  const ACTIVE_STATES = new Set(["init", "thinking", "approved", "executing", "reviewing"]);
  const busy = conversations.some((c) => ACTIVE_STATES.has(c.state));

  return (
    <div className="min-h-screen bg-zinc-950 text-white">
      <StatsBanner stats={stats} config={projectConfig} />
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
    </div>
  );
}
