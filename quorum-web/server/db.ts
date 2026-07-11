import { Database } from "bun:sqlite";
import { config, getState, getProjectConfig } from "../config";

// Resolve current project's DB path, falling back to legacy hardcoded path
function getDbPath(): string {
  const state = getState();
  if (state.currentProject) {
    const pc = getProjectConfig(state.currentProject);
    return pc.dbPath;
  }
  return config.dbPath;
}

// Fresh connection for cross-process reads (WAL snapshot isolation)
export function freshQuery<T>(sql: string, params?: any[]): T[] {
  const fresh = new Database(getDbPath(), { readonly: true });
  const result = params
    ? fresh.query(sql).all(...params) as T[]
    : fresh.query(sql).all() as T[];
  fresh.close();
  return result;
}

// Write connection for targeted updates (e.g., editing proposal before approve)
export function dbWrite(sql: string, params: any[]): void {
  const writable = new Database(getDbPath());
  writable.query(sql).run(...params);
  writable.close();
}

export interface Conversation {
  id: number;
  goal: string;
  state: string;
  round: number;
  max_rounds: number;
  budget_usd: number;
  spent_usd: number;
  created_at: string;
  completed_at: string | null;
  paused_reason: string | null;
  current_agent: string | null;
  path_index: number;
}

export interface Task {
  id: number;
  agent: string;
  task_type: string;
  status: string;
  prompt: string;
  result: string | null;
  token_in: number | null;
  token_out: number | null;
  cost: number | null;
  error: string | null;
  created_at: string;
  started_at: string | null;
  completed_at: string | null;
  conversation_id: number | null;
  session_id: string | null;
}

export function getConversations(): Conversation[] {
  const db = new Database(getDbPath(), { readonly: true });
  const result = db.query("SELECT * FROM conversations ORDER BY id DESC").all() as Conversation[];
  db.close();
  return result;
}

export function getConversation(id: number): Conversation | null {
  const db = new Database(getDbPath(), { readonly: true });
  const result = db.query("SELECT * FROM conversations WHERE id = ?").get(id) as Conversation | null;
  db.close();
  return result;
}

export function getTasksForConversation(id: number): Task[] {
  const db = new Database(getDbPath(), { readonly: true });
  const result = db
    .query("SELECT * FROM tasks WHERE conversation_id = ? ORDER BY id ASC")
    .all(id) as Task[];
  db.close();
  return result;
}

// Brainstorm staging — the rows behind the human approval gate. The daemon
// stages every VAULT_UPDATE here while a brainstorm awaits `respond`; rows
// are flushed once the gate clears. This is the source of truth for the
// approval manifest (better than parsing the leader's prose).
export interface PendingVaultUpdate {
  id: number;
  conversation_id: number;
  agent_id: string;
  role: string;
  mode: string;
  path: string;
  content: string;
  created_at: string;
}

export function getPendingVaultUpdates(conversationId: number): PendingVaultUpdate[] {
  try {
    return freshQuery<PendingVaultUpdate>(
      "SELECT * FROM pending_vault_updates WHERE conversation_id = ? ORDER BY id ASC",
      [conversationId],
    );
  } catch {
    return []; // table absent on older DBs
  }
}

export function getStats() {
  const db = new Database(getDbPath(), { readonly: true });
  const totalConversations = db
    .query("SELECT COUNT(*) as count FROM conversations")
    .get() as { count: number };
  const totalCost = db
    .query("SELECT COALESCE(SUM(cost), 0) as total FROM tasks")
    .get() as { total: number };
  const activeTasks = db
    .query("SELECT COUNT(*) as count FROM tasks WHERE status = 'active'")
    .get() as { count: number };
  const activeConversations = db
    .query("SELECT COUNT(*) as count FROM conversations WHERE state NOT IN ('done', 'closed')")
    .get() as { count: number };
  db.close();

  return {
    total_conversations: totalConversations.count,
    total_cost_usd: Math.round(totalCost.total * 100) / 100,
    active_tasks: activeTasks.count,
    active_conversations: activeConversations.count,
  };
}
