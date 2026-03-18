import type { Conversation, Task, Stats, ProjectConfig, ProjectState, Team, Agent, BudgetInfo, AgentCost } from "./types";

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

export async function startConversation(goal: string, team?: string) {
  const res = await fetch(`${BASE}/converse`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ goal, team: team ?? undefined }),
  });
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

export async function updateBudget(id: number, budget_usd: number) {
  const res = await fetch(`${BASE}/conversations/${id}/budget`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ budget_usd }),
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

export async function fetchTeams(): Promise<Team[]> {
  const res = await fetch(`${BASE}/teams`);
  return res.json();
}

export async function fetchAgents(): Promise<Agent[]> {
  const res = await fetch(`${BASE}/agents`);
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

export async function updateConfig(updates: Record<string, string | number | boolean>) {
  const res = await fetch(`${BASE}/config`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(updates),
  });
  return res.json();
}
