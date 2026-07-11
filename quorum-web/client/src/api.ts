import type {
  Conversation,
  Task,
  Stats,
  ProjectConfig,
  ProjectState,
  Agent,
  BudgetInfo,
  AgentCost,
  Flight,
  PlanPayload,
  PlanResult,
  PendingVaultUpdate,
} from "./types";

const BASE = "/api";

export async function fetchConversations(): Promise<Conversation[]> {
  const res = await fetch(`${BASE}/conversations`);
  return res.json();
}

export async function fetchConversation(id: number): Promise<Conversation & { tasks: Task[] }> {
  const res = await fetch(`${BASE}/conversations/${id}`);
  return res.json();
}

export async function fetchStats(): Promise<Stats> {
  const res = await fetch(`${BASE}/stats`);
  return res.json();
}

export async function fetchConfig(): Promise<ProjectConfig> {
  const res = await fetch(`${BASE}/config`);
  return res.json();
}

export type ConversationMode = "generic" | "brainstorm";

export async function startConversation(
  goal: string,
  mode?: ConversationMode,
) {
  const res = await fetch(`${BASE}/converse`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      goal,
      mode: mode ?? undefined,
    }),
  });
  return res.json();
}

// Staged VAULT_UPDATEs behind a brainstorm's approval gate.
export async function fetchPendingVault(id: number): Promise<PendingVaultUpdate[]> {
  const res = await fetch(`${BASE}/conversations/${id}/pending-vault`);
  if (!res.ok) return [];
  return res.json();
}

export async function respondToLeader(id: number, text: string) {
  const res = await fetch(`${BASE}/respond/${id}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ text }),
  });
  return res.json();
}

export async function closeConversation(id: number) {
  const res = await fetch(`${BASE}/close/${id}`, { method: "POST" });
  return res.json();
}

export async function resumeConversation(id: number) {
  const res = await fetch(`${BASE}/resume/${id}`, { method: "POST" });
  return res.json();
}

// Raise a conversation's max_rounds (real per-conversation limiter) and resume
// if it was paused.
export async function updateMaxRounds(id: number, max_rounds: number) {
  const res = await fetch(`${BASE}/conversations/${id}/max-rounds`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ max_rounds }),
  });
  return res.json();
}

// Phase 14 T6 — "What's going on?" recap. Shells `quorum ask --agent recap`
// against the active project on the server. Multi-minute call; the caller shows
// a loading state. On CLI failure the server returns { error } with the real
// message (e.g. recap knower not set up).
export async function askRecap(
  prompt: string,
): Promise<{ answer?: string; error?: string }> {
  const res = await fetch(`${BASE}/recap`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ prompt }),
  });
  return res.json();
}

export async function fetchProjects(): Promise<ProjectState> {
  const res = await fetch(`${BASE}/projects`);
  return res.json();
}

export async function selectProject(path: string): Promise<{ success: boolean; path?: string; error?: string }> {
  const res = await fetch(`${BASE}/projects/select`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ path }),
  });
  return res.json();
}

export async function initProject(path: string): Promise<{ success: boolean; output?: string; error?: string }> {
  const res = await fetch(`${BASE}/init`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ path }),
  });
  return res.json();
}

export async function fetchAgents(): Promise<Agent[]> {
  const res = await fetch(`${BASE}/agents`);
  return res.json();
}

export async function createAgent(params: {
  role?: string;
  specialty?: string;
  name: string;
  description?: string;
  targetDir?: string;
  skill?: string;
}): Promise<{ success: boolean; output?: string; error?: string }> {
  const res = await fetch(`${BASE}/agents`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(params),
  });
  return res.json();
}

export async function fetchAgentContext(agentId: string): Promise<{ id: string; content: string }> {
  const res = await fetch(`${BASE}/agents/${agentId}/context`);
  if (!res.ok) return { id: agentId, content: "" };
  return res.json();
}

export async function updateAgentContext(agentId: string, content: string): Promise<{ success: boolean; error?: string }> {
  const res = await fetch(`${BASE}/agents/${agentId}/context`, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ content }),
  });
  return res.json();
}

export async function fetchBudget(): Promise<BudgetInfo> {
  const res = await fetch(`${BASE}/budget`);
  return res.json();
}

export async function updateWindowBudget(budget_usd?: number, remaining_minutes?: number) {
  const res = await fetch(`${BASE}/budget`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      ...(budget_usd != null && { budget_usd }),
      ...(remaining_minutes != null && { remaining_minutes }),
    }),
  });
  return res.json();
}

export async function fetchAgentCosts(): Promise<AgentCost[]> {
  const res = await fetch(`${BASE}/budget/agents`);
  return res.json();
}

export async function fetchDaemonStatus(): Promise<{ running: boolean }> {
  const res = await fetch(`${BASE}/daemon/status`);
  return res.json();
}

// Docent — our own knowledge agent (quorum-own-agent/): grounded, cited
// answers from the knower vaults in seconds. Multi-call agentic loop on the
// server; the caller shows a loading state.
export async function askDocent(
  question: string,
  singleShot = false,
): Promise<{ answer?: string; steps?: string[]; error?: string }> {
  const res = await fetch(`${BASE}/docent/ask`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ question, singleShot }),
  });
  return res.json();
}

// ── autopilot (second engine) ─────────────────────────────────────────
// The web prepares + reviews flights; a terminal runs them. See server notes.

export async function fetchFlights(): Promise<Flight[]> {
  const res = await fetch(`${BASE}/autopilot/flights`);
  const data = await res.json();
  return data.flights ?? [];
}

export async function submitFlightPlan(payload: PlanPayload): Promise<PlanResult> {
  const res = await fetch(`${BASE}/autopilot/plan`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  });
  return res.json();
}

export async function setFlightsReviewed(ids: string[], reviewed: boolean) {
  const res = await fetch(`${BASE}/autopilot/reviewed`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ ids, reviewed }),
  });
  return res.json();
}

// Web-first: `quorum knower refresh` as a button. Spawns detached on the
// server; the passes appear in the conversations list as they run.
export async function refreshKnowers(
  knower?: string,
): Promise<{ started?: boolean; note?: string; error?: string }> {
  const res = await fetch(`${BASE}/knower/refresh`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ knower }),
  });
  return res.json();
}

export async function updateConfig(updates: Record<string, string | number | boolean>) {
  const res = await fetch(`${BASE}/config`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(updates),
  });
  return res.json();
}
