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

export interface ProjectState {
  current: string | null;
  recent: string[];
}

export interface Agent {
  id: string;
  name: string;
  role: string;
  description: string;
  skill_file: string | null;
}

export interface BudgetInfo {
  budget_usd: number;
  window_hours: number;
  window_start: string | null;
  spent_usd: number;
  remaining_usd: number;
  remaining_minutes: number;
  is_expired: boolean;
}

export interface AgentCost {
  agent: string;
  tasks: number;
  total_cost: number;
  avg_cost: number;
}

export interface ProjectConfig {
  config_path: string;
  daemon: {
    target_dir: string | null;
    pid_file: string | null;
    data_dir: string | null;
    log_level: string | null;
  };
  budget: {
    window_budget_usd: number | null;
    window_hours: number | null;
  };
  conversations: {
    leader: string | null;
    default_path: string | null;
    default_max_turns: number | null;
  };
  agents: string[];
}
