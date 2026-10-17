import type {
  Candidate,
  Feedback,
  Replay,
  Role,
  RoleSummary,
  CastInfo,
  Derived,
  EpisodeMeta,
  GenerateRequest,
  Health,
  Project,
  ProjectCheck,
  SaveResult,
  Script,
  StudyCandidate,
  StudyDay,
  StudyExcerpt,
  StudyTrack,
  StudyTrackSummary,
  StudyTrackView,
} from './types';

export class ApiError extends Error {
  constructor(
    public status: number,
    message: string,
    public errors: string[] = [],
  ) {
    super(message);
  }
}

async function call<T>(method: string, path: string, body?: unknown): Promise<T> {
  const res = await fetch(`/api${path}`, {
    method,
    headers: body === undefined ? undefined : { 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const text = await res.text();
  const data = text ? JSON.parse(text) : null;
  if (!res.ok) throw new ApiError(res.status, data?.error ?? res.statusText, data?.errors ?? []);
  return data as T;
}

export const api = {
  health: () => call<Health>('GET', '/health'),
  episodes: () => call<{ episodes: EpisodeMeta[] }>('GET', '/episodes').then((r) => r.episodes),
  episode: (id: string) => call<{ meta: EpisodeMeta; script: Script | null }>('GET', `/episodes/${id}`),
  keep: (id: string, kept: boolean) => call<EpisodeMeta>('POST', `/episodes/${id}/keep`, { kept }),
  remove: (id: string) => call<{ deleted: string }>('DELETE', `/episodes/${id}`),
  scriptMd: async (id: string) => {
    const res = await fetch(`/api/episodes/${id}/script.md`);
    if (!res.ok) throw new ApiError(res.status, 'no script yet');
    return res.text();
  },
  scriptUrl: (id: string) => `/api/episodes/${id}/script.md?download=1`,
  roomMd: async (id: string) => {
    const res = await fetch(`/api/episodes/${id}/room.md`);
    if (!res.ok) throw new ApiError(res.status, 'no room transcript');
    return res.text();
  },
  roomUrl: (id: string) => `/api/episodes/${id}/room.md?download=1`,
  audioUrl: (id: string) => `/api/episodes/${id}/audio`,
  generate: (req: GenerateRequest) => call<{ job: string }>('POST', '/generate', req),
  cancel: (job: string) => call('POST', `/jobs/${job}/cancel`),
  projects: () => call<{ projects: Project[] }>('GET', '/projects').then((r) => r.projects),
  checkProject: (spec: Partial<Project> & { editing?: string }) => call<ProjectCheck>('POST', '/projects/check', spec),
  candidates: () => call<{ repos: Candidate[]; folders: string[] }>('GET', '/projects/candidates'),
  derive: (from: { repo?: string; folder?: string }) => call<Derived>('POST', '/projects/derive', from),
  addProject: (spec: Omit<Project, 'active'>) => call<SaveResult>('POST', '/projects', spec),
  setActive: (id: string, active: boolean) => call<SaveResult>('PATCH', `/projects/${id}`, { active }),
  editProject: (id: string, fields: Partial<Project>) => call<SaveResult>('PATCH', `/projects/${id}`, fields),
  cast: () => call<CastInfo>('GET', '/cast'),
  feedback: (id: string) => call<{ feedback: Feedback[] }>('GET', `/episodes/${id}/feedback`).then((r) => r.feedback),
  vote: (id: string, f: { target: 'turn' | 'line'; n: number; vote: 'up' | 'down'; note?: string }) =>
    call<Feedback>('POST', `/episodes/${id}/feedback`, f),
  unvote: (id: string, fid: string) => call('DELETE', `/episodes/${id}/feedback/${fid}`),
  roles: () => call<{ roles: RoleSummary[] }>('GET', '/roles').then((r) => r.roles),
  role: (sid: string) => call<Role>('GET', `/roles/${sid}`),
  saveRole: (sid: string, text: string) => call<Role>('PUT', `/roles/${sid}`, { text }),
  replay: (sid: string, episode: string, turn: number, text: string) =>
    call<Replay>('POST', `/roles/${sid}/replay`, { episode, turn, text }),
  addSpecialty: (s: { id: string; name: string; voice: string; pan: string; angle: string }) =>
    call<CastInfo>('POST', '/cast', s),
  tracks: () => call<{ tracks: StudyTrackSummary[] }>('GET', '/study').then((r) => r.tracks),
  track: (id: string) => call<StudyTrackView>('GET', `/study/${id}`),
  studyDay: (id: string, date: string) => call<StudyDay>('GET', `/study/${id}/days/${date}`),
  studyCandidates: () => call<{ repos: StudyCandidate[] }>('GET', '/study/candidates').then((r) => r.repos),
  newTrack: (t: { id: string; project: string; repo: string; subtree?: string; goal: string }) =>
    call<StudyTrack>('POST', '/study', t),
  studyStart: (id: string) => call<{ job: string }>('POST', `/study/${id}/start`, {}),
  studySay: (id: string, message: string) => call<{ job: string }>('POST', `/study/${id}/say`, { message }),
  studyEnd: (id: string) => call<{ job: string }>('POST', `/study/${id}/end`, {}),
  studyFinish: (id: string) => call<StudyTrack>('POST', `/study/${id}/finish`, {}),
  studyDelete: (id: string) => call<{ deleted: string }>('DELETE', `/study/${id}`),
  studyFile: (id: string, path: string, line: number, end?: number | null) =>
    call<StudyExcerpt>('GET', `/study/${id}/file?path=${encodeURIComponent(path)}&line=${line}${end ? `&end=${end}` : ''}`),
};
