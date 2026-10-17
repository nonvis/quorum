export type Kind = 'recap' | 'brainstorm';
export type Status = 'generating' | 'voicing' | 'ready' | 'unvoiced' | 'failed';

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
  | { event: 'status'; id: string; status: Status }
  | { event: 'done'; id: string; status: Status; duration?: number }
  | { event: 'error'; message: string; id?: string; status?: string }
  | { event: 'exit'; code: number };

export type GenerateRequest =
  | { kind: 'recap'; project: string; range: string; with_operator?: boolean }
  | { kind: 'brainstorm'; project: string; question: string; specialties: string[]; context: string[] };
