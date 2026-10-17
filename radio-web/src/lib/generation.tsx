import { createContext, useCallback, useContext, useEffect, useRef, useState, type ReactNode } from 'react';
import { useQueryClient } from '@tanstack/react-query';
import { toast } from 'sonner';
import { api, ApiError } from './api';
import type { GenerateRequest, JobEvent, Kind, Line, Status } from './types';

export type Phase = 'idle' | 'starting' | 'generating' | 'voicing' | 'done' | 'failed' | 'cancelled';

export interface Generation {
  phase: Phase;
  job: string | null;
  kind: Kind | null;
  project: string | null;
  title: string | null;
  episode: string | null;
  status: Status | null;
  lines: Line[];
  logs: string[];
  error: string | null;
  startedAt: number | null;
}

interface Ctl extends Generation {
  start: (req: GenerateRequest, title: string) => Promise<void>;
  cancel: () => Promise<void>;
  dismiss: () => void;
}

const IDLE: Generation = {
  phase: 'idle', job: null, kind: null, project: null, title: null, episode: null, status: null,
  lines: [], logs: [], error: null, startedAt: null,
};
const Ctx = createContext<Ctl | null>(null);

export function GenerationProvider({ children }: { children: ReactNode }) {
  const qc = useQueryClient();
  const [g, set] = useState<Generation>(IDLE);
  const es = useRef<EventSource | null>(null);

  const refresh = useCallback(() => {
    void qc.invalidateQueries({ queryKey: ['episodes'] });
  }, [qc]);

  const attach = useCallback(
    (job: string) => {
      es.current?.close();
      const src = new EventSource(`/api/jobs/${job}/events`);
      es.current = src;
      src.onmessage = (m) => {
        const ev = JSON.parse(m.data) as JobEvent;
        set((p) => {
          switch (ev.event) {
            case 'started': // a reconnect replays the job from here
              return { ...p, episode: ev.id, kind: ev.kind, project: ev.project, phase: 'generating', lines: [], logs: [] };
            case 'line': {
              const lines = p.lines.filter((l) => l.i !== ev.line.i);
              lines.push(ev.line);
              lines.sort((a, b) => a.i - b.i);
              return { ...p, lines };
            }
            case 'log':
              return { ...p, logs: [...p.logs, ev.text].slice(-200) };
            case 'status':
              return { ...p, status: ev.status, phase: ev.status === 'voicing' ? 'voicing' : p.phase };
            case 'done':
              return { ...p, status: ev.status, phase: 'done' };
            case 'error':
              return { ...p, error: ev.message };
            case 'exit':
              return { ...p, phase: ev.code === 0 ? 'done' : p.phase === 'cancelled' ? 'cancelled' : 'failed' };
          }
          return p;
        });
        if (ev.event === 'started' || ev.event === 'done') refresh();
        if (ev.event === 'exit') {
          src.close();
          refresh();
          if (ev.code === 0) toast.success('On air', { description: 'The episode is ready to play.' });
        }
      };
      src.onerror = () => {
        if (src.readyState === EventSource.CLOSED) refresh();
      };
    },
    [refresh],
  );

  // A page reload while a generation runs re-attaches to it.
  useEffect(() => {
    api.health().then((h) => {
      const j = h.running[0];
      if (j) {
        set({ ...IDLE, phase: 'generating', job: j.id, kind: j.kind, project: j.project, episode: j.episode,
              title: j.kind === 'recap' ? `${j.project} recap` : 'Brainstorm', startedAt: Date.parse(j.started) });
        attach(j.id);
      }
    }).catch(() => undefined);
    return () => es.current?.close();
  }, [attach]);

  const start = useCallback<Ctl['start']>(
    async (req, title) => {
      set({ ...IDLE, phase: 'starting', kind: req.kind, project: req.project, title, startedAt: Date.now() });
      try {
        const { job } = await api.generate(req);
        set((p) => ({ ...p, job, phase: 'generating' }));
        attach(job);
      } catch (e) {
        const msg = e instanceof ApiError ? e.message : String(e);
        set((p) => ({ ...p, phase: 'failed', error: msg }));
        toast.error('Could not start', { description: msg });
      }
    },
    [attach],
  );

  const cancel = useCallback(async () => {
    if (!g.job) return;
    set((p) => ({ ...p, phase: 'cancelled' }));
    es.current?.close();
    await api.cancel(g.job).catch(() => undefined);
    refresh();
  }, [g.job, refresh]);

  const dismiss = useCallback(() => set(IDLE), []);

  return <Ctx.Provider value={{ ...g, start, cancel, dismiss }}>{children}</Ctx.Provider>;
}

export function useGeneration(): Ctl {
  const c = useContext(Ctx);
  if (!c) throw new Error('useGeneration outside GenerationProvider');
  return c;
}

export const running = (p: Phase) => p === 'starting' || p === 'generating' || p === 'voicing';
