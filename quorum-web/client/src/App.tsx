import { useState, useCallback, useEffect } from "react";
import type { Conversation, Stats } from "./types";
import { fetchConversations, fetchStats } from "./api";
import { useSSE } from "./hooks/useSSE";
import { StatsBanner } from "./components/StatsBanner";
import { PromptInput } from "./components/PromptInput";
import { ConversationCard } from "./components/ConversationCard";

export default function App() {
  const [conversations, setConversations] = useState<Conversation[]>([]);
  const [stats, setStats] = useState<Stats | null>(null);

  const refresh = useCallback(async () => {
    const [convs, st] = await Promise.all([fetchConversations(), fetchStats()]);
    setConversations(convs);
    setStats(st);
  }, []);

  // Initial load
  useEffect(() => {
    refresh();
  }, [refresh]);

  // SSE for real-time updates
  useSSE((convs) => {
    setConversations(convs);
    fetchStats().then(setStats);
  });

  return (
    <div className="min-h-screen bg-zinc-950 text-white">
      <StatsBanner stats={stats} />
      <PromptInput onSubmit={refresh} />

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
