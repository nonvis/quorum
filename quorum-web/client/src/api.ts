import type { Conversation, Task, Stats, ProjectConfig } from "./types";

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

export async function startConversation(goal: string, autoApprove = false) {
  const res = await fetch(`${BASE}/converse`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ goal, autoApprove }),
  });
  return res.json();
}

export async function gateApprove(id: number, proposal?: string) {
  const res = await fetch(`${BASE}/gate/${id}/approve`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(proposal ? { proposal } : {}),
  });
  return res.json();
}

export async function gateReject(id: number) {
  const res = await fetch(`${BASE}/gate/${id}/reject`, { method: "POST" });
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

export async function updateConfig(updates: Record<string, string | number | boolean>) {
  const res = await fetch(`${BASE}/config`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(updates),
  });
  return res.json();
}
