import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from 'react';
import { api } from './api';
import type { EpisodeMeta, Script } from './types';

interface PlayerState {
  episode: EpisodeMeta | null;
  script: Script | null;
  playing: boolean;
  time: number;
  duration: number;
  rate: number;
}

interface Player extends PlayerState {
  load: (episode: EpisodeMeta, script: Script | null, opts?: { autoplay?: boolean; at?: number }) => void;
  toggle: () => void;
  seek: (t: number) => void;
  skip: (d: number) => void;
  setRate: (r: number) => void;
  close: () => void;
}

const Ctx = createContext<Player | null>(null);
const RATES = [1, 1.15, 1.3, 1.5];

export function PlayerProvider({ children }: { children: ReactNode }) {
  const audio = useRef<HTMLAudioElement | null>(null);
  const [s, set] = useState<PlayerState>({ episode: null, script: null, playing: false, time: 0, duration: 0, rate: 1 });

  if (!audio.current && typeof Audio !== 'undefined') audio.current = new Audio();

  useEffect(() => {
    const a = audio.current;
    if (!a) return;
    const sync = () =>
      set((p) => ({ ...p, playing: !a.paused, time: a.currentTime, duration: Number.isFinite(a.duration) ? a.duration : p.duration }));
    const evs = ['play', 'pause', 'timeupdate', 'durationchange', 'ended', 'seeked', 'loadedmetadata'] as const;
    evs.forEach((e) => a.addEventListener(e, sync));
    return () => evs.forEach((e) => a.removeEventListener(e, sync));
  }, []);

  const load = useCallback<Player['load']>((episode, script, opts) => {
    const a = audio.current;
    if (!a) return;
    const same = s.episode?.id === episode.id;
    set((p) => ({ ...p, episode, script, duration: episode.duration ?? script?.audio?.duration ?? 0 }));
    if (!same) {
      a.src = api.audioUrl(episode.id);
      a.playbackRate = s.rate;
    }
    if (opts?.at !== undefined) a.currentTime = opts.at;
    if (opts?.autoplay) void a.play().catch(() => undefined);
  }, [s.episode?.id, s.rate]);

  const value = useMemo<Player>(
    () => ({
      ...s,
      load,
      toggle: () => {
        const a = audio.current;
        if (!a || !s.episode) return;
        if (a.paused) void a.play().catch(() => undefined);
        else a.pause();
      },
      seek: (t) => {
        if (audio.current) audio.current.currentTime = Math.max(0, t);
      },
      skip: (d) => {
        const a = audio.current;
        if (a) a.currentTime = Math.min(Math.max(0, a.currentTime + d), a.duration || Infinity);
      },
      setRate: (r) => {
        if (audio.current) audio.current.playbackRate = r;
        set((p) => ({ ...p, rate: r }));
      },
      close: () => {
        const a = audio.current;
        if (a) {
          a.pause();
          a.removeAttribute('src');
          a.load();
        }
        set((p) => ({ ...p, episode: null, script: null, playing: false, time: 0, duration: 0 }));
      },
    }),
    [s, load],
  );

  // Space toggles play, ←/→ skip 10 s — unless typing.
  useEffect(() => {
    const on = (e: KeyboardEvent) => {
      const t = e.target as HTMLElement | null;
      if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.isContentEditable)) return;
      if (!value.episode) return;
      if (e.code === 'Space') {
        e.preventDefault();
        value.toggle();
      } else if (e.code === 'ArrowLeft') value.skip(-10);
      else if (e.code === 'ArrowRight') value.skip(10);
    };
    window.addEventListener('keydown', on);
    return () => window.removeEventListener('keydown', on);
  }, [value]);

  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}

export function usePlayer(): Player {
  const p = useContext(Ctx);
  if (!p) throw new Error('usePlayer outside PlayerProvider');
  return p;
}

export function nextRate(r: number): number {
  return RATES[(RATES.indexOf(r) + 1) % RATES.length] ?? 1;
}
