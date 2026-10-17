import { useEffect, useState } from 'react';
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { toast } from 'sonner';
import { ArrowLeft, ExternalLink, Play, RotateCcw, Save, ThumbsDown, ThumbsUp, Users } from 'lucide-react';
import { api, ApiError } from '@/lib/api';
import { when } from '@/lib/format';
import { go, href } from '@/lib/route';
import { speakerColor } from '@/lib/speakers';
import type { Replay, RoleFeedback } from '@/lib/types';
import { cn } from '@/lib/utils';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Card } from '@/components/ui/card';
import { Textarea } from '@/components/ui/input';
import { Empty, SpeakerDot } from '@/components/bits';

/** Tune each specialty (the tuning loop): its role file, the votes against it, and a replay of any room turn. */
export function RolesView({ id }: { id?: string }) {
  const roles = useQuery({ queryKey: ['roles'], queryFn: api.roles });
  const list = roles.data ?? [];
  return (
    <div className="mx-auto grid max-w-7xl gap-6 px-4 py-6 lg:grid-cols-[280px_minmax(0,1fr)]">
      <aside className={cn('space-y-2', id && 'hidden lg:block')}>
        <h1 className="px-1 text-xl font-semibold tracking-tight">Roles</h1>
        <p className="px-1 pb-2 text-sm text-muted-fg">How each specialty works. Vote on lines while you listen; tune the role here.</p>
        {list.map((r) => (
          <a
            key={r.id}
            href={href({ view: 'roles', id: r.id })}
            className={cn(
              'flex items-center gap-3 rounded-xl border bg-card px-3 py-2.5 shadow-sm hover:border-muted-fg/40',
              r.id === id && 'border-accent ring-1 ring-accent',
            )}
          >
            <SpeakerDot id={r.id} className="size-2.5" />
            <span className="min-w-0 flex-1">
              <span className="block text-sm font-medium">{r.name}</span>
              <span className="block truncate text-[11px] text-muted-fg">{r.angle ?? r.question ?? 'hosts'}</span>
            </span>
            <span className="flex shrink-0 items-center gap-2 text-[11px] tabular-nums text-muted-fg">
              {r.up > 0 && <span className="inline-flex items-center gap-0.5 text-ok"><ThumbsUp className="size-3" />{r.up}</span>}
              {r.down > 0 && <span className="inline-flex items-center gap-0.5 text-danger"><ThumbsDown className="size-3" />{r.down}</span>}
            </span>
          </a>
        ))}
      </aside>
      <main className={cn('min-w-0', !id && 'hidden lg:block')}>
        {id ? (
          <RoleEditor key={id} sid={id} name={list.find((r) => r.id === id)?.name ?? id} />
        ) : (
          <Card>
            <Empty icon={<Users />} title="Pick a specialty">
              Each one has a role file: its method, what a good answer looks like, what it never says. Your 👎 votes collect here.
            </Empty>
          </Card>
        )}
      </main>
    </div>
  );
}

function RoleEditor({ sid, name }: { sid: string; name: string }) {
  const qc = useQueryClient();
  const q = useQuery({ queryKey: ['role', sid], queryFn: () => api.role(sid) });
  const [draft, setDraft] = useState<string | null>(null);
  const [replays, setReplays] = useState<Record<string, Replay | 'running' | string>>({});
  useEffect(() => setDraft(null), [sid]);
  const text = draft ?? q.data?.text ?? '';
  const dirty = draft !== null && draft !== q.data?.text;
  const save = useMutation({
    mutationFn: () => api.saveRole(sid, text),
    onSuccess: (r) => {
      toast.success(`${name} tuned`, { description: r.committed ? `Committed ${r.committed} to quorum.` : 'Saved.' });
      setDraft(null);
      void qc.invalidateQueries({ queryKey: ['role', sid] });
      void qc.invalidateQueries({ queryKey: ['roles'] });
    },
    onError: (e) => toast.error('Not saved', { description: e instanceof ApiError ? e.message : String(e) }),
  });
  const replay = async (f: RoleFeedback) => {
    if (f.replay_turn === null) return;
    setReplays((r) => ({ ...r, [f.id + f.episode]: 'running' }));
    try {
      const r = await api.replay(sid, f.episode, f.replay_turn, text);
      setReplays((x) => ({ ...x, [f.id + f.episode]: r }));
    } catch (e) {
      setReplays((x) => ({ ...x, [f.id + f.episode]: e instanceof ApiError ? e.message : String(e) }));
    }
  };

  if (q.isLoading) return <div className="h-96 animate-pulse rounded-xl bg-muted" />;
  const fb = q.data?.feedback ?? [];
  return (
    <div className="space-y-5">
      <Button variant="ghost" size="sm" className="-ml-2 lg:hidden" onClick={() => go({ view: 'roles' })}>
        <ArrowLeft /> Roles
      </Button>
      <Card className="p-5">
        <div className="mb-3 flex flex-wrap items-center gap-2">
          <SpeakerDot id={sid} className="size-3" />
          <h2 className="text-lg font-semibold" style={{ color: speakerColor(sid) }}>{name}</h2>
          <code className="font-mono text-[11px] text-muted-fg">radio/roles/{sid}.md</code>
          {dirty && <Badge variant="accent">unsaved</Badge>}
          <div className="ml-auto flex gap-2">
            <Button size="sm" variant="ghost" disabled={!dirty} onClick={() => setDraft(null)}>
              <RotateCcw /> Revert
            </Button>
            <Button size="sm" disabled={!dirty || save.isPending} onClick={() => save.mutate()}>
              <Save /> Save and commit
            </Button>
          </div>
        </div>
        <Textarea
          value={text}
          onChange={(e) => setDraft(e.target.value)}
          rows={Math.min(30, Math.max(12, text.split('\n').length + 2))}
          className="font-mono text-xs leading-relaxed"
          spellCheck={false}
        />
        <p className="mt-2 text-xs text-muted-fg">
          The text before the first <code>## Room</code> / <code>## Recap</code> applies to both; each section only to its kind of
          episode. A replay uses this draft, saved or not.
        </p>
      </Card>

      <Card className="p-5">
        <h3 className="mb-3 text-xs font-semibold uppercase tracking-wider text-muted-fg">Votes on {name} ({fb.length})</h3>
        {fb.length === 0 ? (
          <p className="text-sm text-muted-fg">No votes yet. While you listen, use 👍 / 👎 on a line or a room turn.</p>
        ) : (
          <ul className="space-y-3">
            {fb.map((f) => {
              const r = replays[f.id + f.episode];
              return (
                <li key={f.id + f.episode} className="rounded-lg border p-3">
                  <div className="flex flex-wrap items-center gap-2 text-xs text-muted-fg">
                    {f.vote === 'up' ? <ThumbsUp className="size-3.5 text-ok" /> : <ThumbsDown className="size-3.5 text-danger" />}
                    <a href={href({ view: 'listen', id: f.episode })} className="inline-flex items-center gap-1 font-medium text-fg hover:underline">
                      {f.title} <ExternalLink className="size-3" />
                    </a>
                    <span>· {f.target === 'turn' ? `room turn ${f.n}` : `line ${f.n}`} · {when(f.at)}</span>
                    {f.replay_turn !== null && (
                      <Button size="sm" variant="outline" className="ml-auto h-7" disabled={r === 'running'} onClick={() => replay(f)}>
                        <Play /> {r === 'running' ? 'Replaying…' : 'Replay with this role'}
                      </Button>
                    )}
                  </div>
                  {f.note && <p className="mt-1.5 text-sm font-medium">“{f.note}”</p>}
                  {!r || r === 'running' ? (
                    <p className="mt-1.5 line-clamp-3 text-sm text-muted-fg">{f.excerpt}</p>
                  ) : typeof r === 'string' ? (
                    <p className="mt-1.5 text-sm text-danger">{r}</p>
                  ) : (
                    <div className="mt-2 grid gap-2 md:grid-cols-2">
                      <div className="rounded-md bg-muted/50 p-2.5">
                        <div className="mb-1 text-[11px] font-semibold uppercase text-muted-fg">Before</div>
                        <p className="whitespace-pre-wrap text-xs leading-relaxed">{r.old}</p>
                      </div>
                      <div className="rounded-md bg-accent-soft/40 p-2.5">
                        <div className="mb-1 text-[11px] font-semibold uppercase text-accent">
                          With this role{r.cost ? ` · $${r.cost.toFixed(2)}` : ''}
                        </div>
                        <p className="whitespace-pre-wrap text-xs leading-relaxed">{r.new}</p>
                      </div>
                    </div>
                  )}
                </li>
              );
            })}
          </ul>
        )}
      </Card>
    </div>
  );
}
