import type {
  CastInfo,
  EpisodeMeta,
  GenerateRequest,
  Health,
  Project,
  ProjectCheck,
  SaveResult,
  Script,
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
  audioUrl: (id: string) => `/api/episodes/${id}/audio`,
  generate: (req: GenerateRequest) => call<{ job: string }>('POST', '/generate', req),
  cancel: (job: string) => call('POST', `/jobs/${job}/cancel`),
  projects: () => call<{ projects: Project[] }>('GET', '/projects').then((r) => r.projects),
  checkProject: (spec: Partial<Project>) => call<ProjectCheck>('POST', '/projects/check', spec),
  addProject: (spec: Omit<Project, 'active'>) => call<SaveResult>('POST', '/projects', spec),
  setActive: (id: string, active: boolean) => call<SaveResult>('PATCH', `/projects/${id}`, { active }),
  editProject: (id: string, fields: Partial<Project>) => call<SaveResult>('PATCH', `/projects/${id}`, fields),
  cast: () => call<CastInfo>('GET', '/cast'),
  addSpecialty: (s: { id: string; name: string; voice: string; pan: string; angle: string }) =>
    call<CastInfo>('POST', '/cast', s),
};
