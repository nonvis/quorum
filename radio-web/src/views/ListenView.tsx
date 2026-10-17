import { useMemo, useState } from 'react';
import { useQuery } from '@tanstack/react-query';
import { Headphones, Star } from 'lucide-react';
import { api } from '@/lib/api';
import { bucket, clock, retention, when } from '@/lib/format';
import { usePlayer } from '@/lib/player';
import { go, href } from '@/lib/route';
import type { EpisodeMeta, Kind } from '@/lib/types';
import { cn } from '@/lib/utils';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Card } from '@/components/ui/card';
import { Empty, KindIcon, OnAir } from '@/components/bits';
import { EpisodeDetail } from './EpisodeDetail';

const ORDER = ['Today', 'Yesterday', 'This week', 'Earlier'];

export function ListenView({ id }: { id?: string }) {
  const q = useQuery({ queryKey: ['episodes'], queryFn: api.episodes, refetchInterval: 15_000 });
  const [kind, setKind] = useState<Kind | 'all'>('all');
  const [project, setProject] = useState<string>('all');
  const [keptOnly, setKeptOnly] = useState(false);

  const eps = q.data ?? [];
  const projects = useMemo(
    () => [...new Map(eps.map((e) => [e.project, e.project_name ?? e.project])).entries()],
    [eps],
  );
  const shown = eps.filter(
    (e) => (kind === 'all' || e.kind === kind) && (project === 'all' || e.project === project) && (!keptOnly || e.kept),
  );
  const groups = ORDER.map((b) => [b, shown.filter((e) => bucket(e.created) === b)] as const).filter(([, l]) => l.length);

  return (
    <div className="mx-auto grid max-w-7xl gap-6 px-4 py-6 lg:grid-cols-[380px_minmax(0,1fr)]">
      <aside className={cn('min-w-0 space-y-4', id && 'hidden lg:block')}>
        <div className="flex flex-wrap items-center gap-1.5">
          {(['all', 'recap', 'brainstorm'] as const).map((k) => (
            <Chip key={k} on={kind === k} onClick={() => setKind(k)}>
              {k === 'all' ? 'All' : k === 'recap' ? 'Recaps' : 'Brainstorms'}
            </Chip>
          ))}
          <Chip on={keptOnly} onClick={() => setKeptOnly(!keptOnly)}>
            <Star className="size-3" /> Kept
          </Chip>
        </div>
        {projects.length > 1 && (
          <div className="flex flex-wrap gap-1.5">
            <Chip on={project === 'all'} onClick={() => setProject('all')}>
              Every project
            </Chip>
            {projects.map(([pid, name]) => (
              <Chip key={pid} on={project === pid} onClick={() => setProject(pid)}>
                {name}
              </Chip>
            ))}
          </div>
        )}
        {q.isLoading ? (
          <div className="space-y-2">
            {[0, 1, 2].map((i) => (
              <div key={i} className="h-20 animate-pulse rounded-xl bg-muted" />
            ))}
          </div>
        ) : shown.length === 0 ? (
          <Card>
            <Empty icon={<Headphones />} title={eps.length ? 'Nothing matches' : 'No episodes yet'}>
              {eps.length ? 'Change the filters above.' : 'Generate a recap of a project’s day, or put one question to the room.'}
              {!eps.length && (
                <div className="mt-4">
                  <Button onClick={() => go({ view: 'generate' })}>Generate one</Button>
                </div>
              )}
            </Empty>
          </Card>
        ) : (
          groups.map(([b, list]) => (
            <section key={b} className="space-y-2">
              <h3 className="px-1 text-xs font-semibold uppercase tracking-wider text-muted-fg">{b}</h3>
              {list.map((e) => (
                <EpisodeRow key={e.id} e={e} selected={e.id === id} />
              ))}
            </section>
          ))
        )}
        <p className="px-1 text-xs text-muted-fg">Episodes you don’t keep are deleted 7 days after they’re made.</p>
      </aside>
      <main className={cn('min-w-0', !id && 'hidden lg:block')}>
        {id ? (
          <EpisodeDetail id={id} />
        ) : (
          <Card className="sticky top-20">
            <Empty icon={<Headphones />} title="Pick an episode">
              Play it, keep it if it’s worth remembering, or export the script into a working session.
            </Empty>
          </Card>
        )}
      </main>
    </div>
  );
}

function Chip({ on, onClick, children }: { on: boolean; onClick: () => void; children: React.ReactNode }) {
  return (
    <button
      type="button"
      onClick={onClick}
      className={cn(
        'inline-flex items-center gap-1 rounded-full border px-3 py-1 text-xs font-medium transition-colors',
        on ? 'border-accent bg-accent-soft text-accent' : 'bg-card text-muted-fg hover:text-fg',
      )}
    >
      {children}
    </button>
  );
}

function EpisodeRow({ e, selected }: { e: EpisodeMeta; selected: boolean }) {
  const p = usePlayer();
  const playing = p.episode?.id === e.id && p.playing;
  const busy = e.status === 'generating' || e.status === 'voicing';
  return (
    <a
      href={href({ view: 'listen', id: e.id })}
      className={cn(
        'block rounded-xl border bg-card p-3 shadow-sm transition-colors hover:border-muted-fg/40',
        selected && 'border-accent ring-1 ring-accent',
      )}
    >
      <div className="flex items-start gap-3">
        <div
          className={cn(
            'mt-0.5 flex size-8 shrink-0 items-center justify-center rounded-lg',
            e.kind === 'brainstorm' ? 'bg-[var(--spk-architect)]/15 text-[var(--spk-architect)]' : 'bg-accent-soft text-accent',
          )}
        >
          <KindIcon kind={e.kind} />
        </div>
        <div className="min-w-0 flex-1">
          <div className="flex items-center gap-2">
            <span className="truncate text-sm font-medium">{e.title}</span>
            {playing && <OnAir live className="shrink-0" />}
          </div>
          <div className="mt-1 flex flex-wrap items-center gap-x-2 gap-y-1 text-xs text-muted-fg">
            <span>{e.project_name ?? e.project}</span>
            <span>·</span>
            <span>{when(e.created)}</span>
            {e.duration ? (
              <>
                <span>·</span>
                <span className="tabular-nums">{clock(e.duration)}</span>
              </>
            ) : null}
          </div>
        </div>
        <div className="flex shrink-0 flex-col items-end gap-1">
          {busy ? (
            <Badge variant="accent">
              <OnAir live /> {e.status}
            </Badge>
          ) : e.status === 'failed' ? (
            <Badge variant="danger">failed</Badge>
          ) : e.kept ? (
            <Star className="size-4 fill-accent text-accent" aria-label="kept" />
          ) : (
            <span className="text-[11px] text-muted-fg">{retention(e)}</span>
          )}
        </div>
      </div>
    </a>
  );
}
