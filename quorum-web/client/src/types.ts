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
  pipeline: string;
  tasks?: Task[];
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
}

export interface Stats {
  total_conversations: number;
  total_cost_usd: number;
  active_tasks: number;
  active_conversations: number;
}

export interface ProjectConfig {
  target_dir: string | null;
  pipeline: string | null;
  config_path: string;
}
