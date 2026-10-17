export type Kind = 'recap' | 'brainstorm';
export type Status = 'generating' | 'editing' | 'voicing' | 'ready' | 'unvoiced' | 'failed';

/** One turn of a brainstorm's room (QR-31): the host routing, a specialist answering or passing, the close. */
export interface RoomTurn {
  n: number;
  speaker: string;
  name?: string;
  kind: 'route' | 'answer' | 'pass' | 'close';
  to?: string;
  prompt?: string;
  text: string;
  verdict?: string;
  threads?: string[];
}

export interface Room {
  question: string;
  chosen: string[];
  turns: RoomTurn[];
  threads: string[];
  calls: number;
  cap: number;
  sessions: Record<string, string>;
}

export interface EpisodeMeta {
  id: string;
  kind: Kind;
  project: string;
  project_name?: string;
  title: string;
  created: string;
  kept: boolean;
  status: Status;
  expires: string | null;
  days_left: number | null;
  has_audio: boolean;
  label?: string;
  range?: string;
  question?: string;
  specialties?: string[];
  context?: string[];
  duration?: number;
  lines?: number;
  calls?: number;
  cap?: number;
  spend?: string | null;
  model?: string | null;
  spoke?: string[];
  threads?: string[] | null;
  error?: string;
  timings?: Record<string, number>;
}

export interface Source {
  id: string;
  kind: string;
  ref: string;
  text?: string;
  subject?: string;
}

export interface Line {
  i: number;
  speaker: string;
  name?: string;
  kind: string;
  text: string;
  label?: 'grounded' | 'inferred' | null;
  source?: Source | null;
  about?: number | null;
  on_air: boolean;
  withdrawn?: boolean;
  verdict?: string;
  evidence?: { cmd: string; ok: boolean; chars: number }[];
}

export interface AudioTiming {
  duration: number;
  lines: { i: number; speaker: string; start: number; end: number }[];
}

export interface Script {
  kind?: Kind;
  lines: Line[];
  threads?: string[];
  question?: string;
  chosen?: string[];
  log?: string[];
  audio?: AudioTiming | null;
  calls?: { total: number; cap: number };
  room?: Room;
}

export interface Project {
  id: string;
  name: string;
  active: boolean;
  notes: string[];
  folders: string[];
  repos: string[];
  concept: string[];
}

export interface ProjectCheck {
  notes: { pattern: string; matches: string[] }[];
  concept: { pattern: string; matches: string[] }[];
  folders: { path: string; exists: boolean }[];
  repos: { name: string; path: string | null; remote: string; own: boolean }[];
  errors: string[];
}

export interface SaveResult {
  committed: string | null;
  pushed: boolean;
  push_error: string | null;
  project?: string;
}

export interface Specialty {
  id: string;
  name: string;
  voice: string;
  pan: string;
  recap: boolean;
  question: string | null;
  angle: string | null;
  default: boolean;
}

export interface CastInfo {
  cast: Specialty[];
  free_voices: string[];
  pans: string[];
}

export interface Health {
  ok: boolean;
  fake: boolean;
  voice: boolean;
  claude: boolean;
  vault: string;
  store: string;
  running: JobView[];
}

export interface JobView {
  id: string;
  kind: Kind;
  project: string;
  episode: string | null;
  done: boolean;
  code: number | null;
  started: string;
}

export type JobEvent =
  | { event: 'started'; id: string; kind: Kind; project: string }
  | { event: 'line'; line: Line }
  | { event: 'log'; text: string }
  | { event: 'room'; turn: RoomTurn }
  | { event: 'status'; id: string; status: Status }
  | { event: 'done'; id: string; status: Status; duration?: number }
  | { event: 'error'; message: string; id?: string; status?: string }
  | { event: 'exit'; code: number };

export type GenerateRequest =
  | { kind: 'recap'; project: string; range: string; with?: string[] }
  | { kind: 'brainstorm'; project: string; question: string; specialties: string[]; context: string[] };

export interface Candidate {
  repo: string;
  path: string;
  folder: string | null;
  notes: number;
}

export interface Derived {
  spec: Omit<Project, 'active'>;
  warnings: string[];
  mentions: Record<string, number>;
  check: ProjectCheck;
}

export interface Feedback {
  id: string;
  target: 'turn' | 'line';
  n: number;
  speaker: string;
  vote: 'up' | 'down';
  note: string;
  at: string;
}

export interface RoleSummary {
  id: string;
  name: string;
  voice: string;
  angle: string | null;
  question: string | null;
  has_role: boolean;
  up: number;
  down: number;
}

export interface RoleFeedback extends Feedback {
  episode: string;
  title: string;
  kind: Kind;
  excerpt: string;
  replay_turn: number | null;
}

export interface Role {
  id: string;
  text: string;
  path: string;
  feedback: RoleFeedback[];
  committed?: string | null;
}

export interface Replay {
  speaker: string;
  turn: number;
  old: string;
  new: string;
  cost: number;
}
