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
  // Already persisted by the daemon and returned via SELECT * — the mode a
  // conversation was started in (generic | brainstorm).
  mode: string;
  no_vault_write?: number;
  team?: string | null;
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

// ── autopilot (second execution engine) ──────────────────────────────
// Read model for a "flight" — an overnight `claude --agent supervisor` run.
// Mirrors quorum-web/server/autopilot.ts; sourced from SUPERVISOR.md +
// .quorum/autopilot/ (checkpoint.md, archive/*.md), per autopilot-protocol v0.3.

export type FlightTaskStatus = "pending" | "in_flight" | "done";
export type FlightStatus = "ready" | "in_flight" | "needs_you" | "complete";

export interface FlightTask {
  index: number;
  title: string;
  status: FlightTaskStatus;
  warn: boolean; // done-but-with-caveat (outcome mentions a skip/failure)
}

export interface FlightOutcome {
  heading: string;
  taskIndex: number | null;
  bullets: string[];
}

export interface MorningReview {
  done: string;
  pending: string;
  blockedOn: string;
  notes: string | null;
}

export interface Flight {
  id: string; // "current" for the live checkpoint, else archive file stem
  source: "checkpoint" | "archive" | "bundled-fixture";
  fixture: boolean;
  project: string;
  name: string;
  mode: string | null;
  specVersion: string | null;
  createdAt: string | null;
  updatedAt: string | null;
  status: FlightStatus;
  tasks: FlightTask[];
  outcomes: FlightOutcome[];
  morningReview: MorningReview | null;
  reviewed: boolean;
  launchCommand: string;
}

// Composer → POST /api/autopilot/plan payload (writes SUPERVISOR.md).
export interface PlanTask {
  title: string;
  agent: string;
  slices: string[];
  doneWhen: string;
}

export interface PlanPayload {
  goal: string;
  mode: "generic" | "brainstorm";
  tasks: PlanTask[];
  maxMajorTasks?: number | null;
  force?: boolean;
}

export interface PlanResult {
  success: boolean;
  needsForce?: boolean;
  error?: string;
  path?: string;
  content?: string;
  launchCommand?: string;
  archivedPrevious?: string | null;
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
