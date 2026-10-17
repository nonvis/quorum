import { useEffect, useMemo, useRef, useState } from 'react';
import { useQuery } from '@tanstack/react-query';
import { Ban, CalendarDays, Check, Lightbulb, Plus, Radio, Sparkles, X } from 'lucide-react';
import { api } from '@/lib/api';
import { useDraft } from '@/lib/draft';
import { kstDate, when, wordCount } from '@/lib/format';
import { running, useGeneration } from '@/lib/generation';
import { usePlayer } from '@/lib/player';
import { go } from '@/lib/route';
import { speakerColor } from '@/lib/speakers';
import type { Project, Specialty } from '@/lib/types';
import { cn } from '@/lib/utils';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Card } from '@/components/ui/card';
import { Checkbox } from '@/components/ui/checkbox';
import { Input, Label, Textarea } from '@/components/ui/input';
import { Switch } from '@/components/ui/switch';
import { Tabs, TabsList, TabsTrigger } from '@/components/ui/tabs';
import { Tip } from '@/components/ui/tooltip';
import { Empty, OnAir, SpeakerDot } from '@/components/bits';
import { ScriptView } from '@/components/ScriptView';
import { RoomView } from '@/components/RoomView';
import { AddSpecialtyDialog } from './AddSpecialtyDialog';

const RANGES = [
  { id: 'today', label: 'Today' },
  { id: 'yesterday', label: 'Yesterday' },
  { id: 'week', label: 'Last 7 days' },
  { id: 'date', label: 'A day…' },
];

export function GenerateView() {
  const { draft, patch } = useDraft();
  const g = useGeneration();
  const projects = useQuery({ queryKey: ['projects'], queryFn: api.projects });
  const active = (projects.data ?? []).filter((p) => p.active);
  const project = active.find((p) => p.id === draft.project) ?? null;

  useEffect(() => {
    if (!draft.project && active.length) patch({ project: active[0].id });
  }, [draft.project, active, patch]);

  const busy = running(g.phase);

  return (
    <div className="mx-auto grid max-w-7xl gap-6 px-4 py-6 lg:grid-cols-[minmax(0,1fr)_minmax(0,1fr)]">
      <div className="min-w-0 space-y-5">
        <Tabs value={draft.kind} onValueChange={(v) => patch({ kind: v as 'recap' | 'brainstorm' })}>
          <TabsList>
            <TabsTrigger value="recap">
              <Radio /> Recap
            </TabsTrigger>
            <TabsTrigger value="brainstorm">
              <Lightbulb /> Brainstorm
            </TabsTrigger>
          </TabsList>
        </Tabs>
        <p className="text-sm text-muted-fg">
          {draft.kind === 'recap'
            ? 'One project’s day on air: what changed, is it real, what was decided, does a user notice.'
            : 'Put one question to the room. The host drives; the specialists answer from their own angles. You get a script.'}
        </p>

        <section className="space-y-2">
          <Label>Project</Label>
          {projects.isLoading ? (
            <div className="h-16 animate-pulse rounded-xl bg-muted" />
          ) : active.length === 0 ? (
            <Card>
              <Empty icon={<Radio />} title="No active project">
                Activate or add one on the Projects page.
              </Empty>
            </Card>
          ) : (
            <div className="grid grid-cols-2 gap-2 sm:grid-cols-3">
              {active.map((p) => (
                <ProjectTile key={p.id} p={p} on={p.id === draft.project} onClick={() => patch({ project: p.id, context: [] })} />
              ))}
            </div>
          )}
        </section>

        {draft.kind === 'recap' ? <RecapForm project={project} busy={busy} /> : <BrainstormForm project={project} busy={busy} />}
      </div>
      <div className="min-w-0">
        <LiveSession />
      </div>
    </div>
  );
}

function ProjectTile({ p, on, onClick }: { p: Project; on: boolean; onClick: () => void }) {
  return (
    <button
      type="button"
      onClick={onClick}
      className={cn(
        'flex flex-col items-start rounded-xl border bg-card px-3 py-2.5 text-left shadow-sm transition-colors hover:border-muted-fg/40',
        on && 'border-accent ring-1 ring-accent',
      )}
    >
      <span className="text-sm font-medium">{p.name}</span>
      <span className="mt-0.5 truncate text-[11px] text-muted-fg">
        {p.repos.length ? p.repos.join(', ') : 'vault notes only'}
      </span>
    </button>
  );
}

function RecapForm({ project, busy }: { project: Project | null; busy: boolean }) {
  const { draft, patch } = useDraft();
  const g = useGeneration();
  const range = draft.range === 'date' ? draft.date : draft.range;
  const label = RANGES.find((r) => r.id === draft.range)?.label ?? draft.date;
  const go_ = () =>
    project &&
    g.start({ kind: 'recap', project: project.id, range, with: draft.invite ?? [] }, `${project.name} · ${label}`);
  return (
    <>
      <section className="space-y-2">
        <Label>Window</Label>
        <div className="flex flex-wrap gap-2">
          {RANGES.map((r) => (
            <Button
              key={r.id}
              size="sm"
              variant={draft.range === r.id ? 'default' : 'outline'}
              onClick={() => patch({ range: r.id, date: r.id === 'date' && !draft.date ? kstDate() : draft.date })}
            >
              {r.id === 'date' && <CalendarDays />}
              {r.label}
            </Button>
          ))}
        </div>
        {draft.range === 'date' && (
          <Input type="date" className="w-48" max={kstDate()} value={draft.date} onChange={(e) => patch({ date: e.target.value })} />
        )}
        <p className="text-xs text-muted-fg">Days are in KST. A range longer than a day is a digest: brief, in themes.</p>
      </section>
      <section className="space-y-2 rounded-xl border bg-card p-4">
        <div className="text-sm font-medium">On air</div>
        <p className="text-xs text-muted-fg">
          The Chronicler opens (where things stand), then the Builder, Verifier, Architect and Historian. Bring in:
        </p>
        <div className="space-y-2 pt-1">
          {[
            { id: 'product', label: 'Product', hint: 'Does a user notice any of it?' },
            { id: 'operator', label: 'Operator', hint: 'What’s waiting on you. It joins by itself when something broke.' },
          ].map((x) => (
            <label key={x.id} className="flex items-center justify-between gap-4">
              <span>
                <span className="flex items-center gap-2 text-sm"><SpeakerDot id={x.id} /> {x.label}</span>
                <span className="block text-xs text-muted-fg">{x.hint}</span>
              </span>
              <Switch
                checked={(draft.invite ?? []).includes(x.id)}
                onCheckedChange={(v) => patch({ invite: v ? [...(draft.invite ?? []), x.id] : (draft.invite ?? []).filter((i) => i !== x.id) })}
              />
            </label>
          ))}
        </div>
      </section>
      <Button size="lg" className="w-full" disabled={!project || busy || (draft.range === 'date' && !draft.date)} onClick={go_}>
        <Sparkles /> {busy ? 'A generation is running' : 'Generate the recap'}
      </Button>
    </>
  );
}

function BrainstormForm({ project, busy }: { project: Project | null; busy: boolean }) {
  const { draft, patch } = useDraft();
  const g = useGeneration();
  const cast = useQuery({ queryKey: ['cast'], queryFn: api.cast });
  const episodes = useQuery({ queryKey: ['episodes'], queryFn: api.episodes });
  const [adding, setAdding] = useState(false);
  const qRef = useRef<HTMLTextAreaElement>(null);

  const able = (cast.data?.cast ?? []).filter((s) => s.id !== 'host' && s.angle);
  const chosen = draft.specialties ?? able.filter((s) => s.default).map((s) => s.id);
  const toggle = (id: string) =>
    patch({ specialties: chosen.includes(id) ? chosen.filter((x) => x !== id) : [...chosen, id] });
  const ctxChoices = useMemo(
    () => (episodes.data ?? []).filter((e) => e.project === project?.id && e.status !== 'failed' && e.status !== 'generating'),
    [episodes.data, project?.id],
  );
  const ctx = draft.context.filter((c) => (episodes.data ?? []).some((e) => e.id === c));
  const question = draft.question.trim();
  const words = wordCount(question);
  const ok = !!project && !!question && chosen.length > 0 && !busy;

  useEffect(() => {
    if (draft.context.length && !question) qRef.current?.focus();
  }, [draft.context.length, question]);

  const go_ = () =>
    project &&
    g.start(
      { kind: 'brainstorm', project: project.id, question, specialties: chosen, context: ctx },
      question.length > 80 ? `${question.slice(0, 78)}…` : question,
    );

  return (
    <>
      <section className="space-y-2">
        <Label htmlFor="q">The question</Label>
        <Textarea
          id="q"
          ref={qRef}
          rows={3}
          placeholder="One question. e.g. Should the moderator gate run before the screen, or after it?"
          value={draft.question}
          maxLength={500}
          onChange={(e) => patch({ question: e.target.value })}
          onKeyDown={(e) => {
            if (e.key === 'Enter' && (e.metaKey || e.ctrlKey) && ok) go_();
          }}
        />
        <div className="flex justify-between text-xs text-muted-fg">
          <span>One question per session; go deeper with a fresh session and this one as context.</span>
          <span className="tabular-nums">{words} words</span>
        </div>
      </section>

      <section className="space-y-2">
        <div className="flex items-center justify-between">
          <Label>In the room</Label>
          <Button size="sm" variant="ghost" onClick={() => setAdding(true)}>
            <Plus /> New specialty
          </Button>
        </div>
        <div className="grid gap-2 sm:grid-cols-2">
          {able.map((s) => (
            <SpecialtyTile key={s.id} s={s} on={chosen.includes(s.id)} onClick={() => toggle(s.id)} />
          ))}
        </div>
        {chosen.length === 0 && <p className="text-xs text-danger">Bring at least one specialty.</p>}
      </section>

      <section className="space-y-2">
        <Label>Context: earlier sessions (optional)</Label>
        {ctxChoices.length === 0 ? (
          <p className="text-xs text-muted-fg">No earlier episodes on this project yet.</p>
        ) : (
          <div className="max-h-56 space-y-1 overflow-y-auto rounded-xl border bg-card p-2">
            {ctxChoices.map((e) => (
              <label key={e.id} className="flex cursor-pointer items-start gap-2.5 rounded-lg px-2 py-1.5 hover:bg-muted">
                <Checkbox
                  className="mt-0.5"
                  checked={ctx.includes(e.id)}
                  onCheckedChange={(v) => patch({ context: v ? [...ctx, e.id] : ctx.filter((x) => x !== e.id) })}
                />
                <span className="min-w-0 flex-1">
                  <span className="block truncate text-sm">{e.kind === 'brainstorm' ? e.question ?? e.title : e.title}</span>
                  <span className="text-[11px] text-muted-fg">
                    {e.kind} · {when(e.created)}
                    {e.kept && ' · kept'}
                  </span>
                </span>
              </label>
            ))}
          </div>
        )}
      </section>

      <Button size="lg" className="w-full" disabled={!ok} onClick={go_}>
        <Sparkles /> {busy ? 'A generation is running' : 'Open the room'}
        <span className="ml-1 hidden text-xs opacity-70 sm:inline">⌘↵</span>
      </Button>
      <AddSpecialtyDialog open={adding} onOpenChange={setAdding} onAdded={(id) => patch({ specialties: [...chosen, id] })} />
    </>
  );
}

function SpecialtyTile({ s, on, onClick }: { s: Specialty; on: boolean; onClick: () => void }) {
  return (
    <button
      type="button"
      onClick={onClick}
      className={cn(
        'flex items-start gap-3 rounded-xl border bg-card p-3 text-left transition-colors hover:border-muted-fg/40',
        on ? 'border-transparent ring-2' : 'opacity-70',
      )}
      style={on ? { ['--tw-ring-color' as string]: speakerColor(s.id) } : undefined}
    >
      <span
        className={cn('mt-0.5 flex size-5 shrink-0 items-center justify-center rounded-full border-2')}
        style={{ borderColor: speakerColor(s.id), background: on ? speakerColor(s.id) : 'transparent' }}
      >
        {on && <Check className="size-3 text-white" strokeWidth={3} />}
      </span>
      <span className="min-w-0">
        <span className="flex items-center gap-2 text-sm font-medium">
          {s.name}
          {!s.recap && <Badge variant="outline">custom</Badge>}
        </span>
        <span className="mt-0.5 block text-xs leading-snug text-muted-fg">{s.angle}</span>
        <Tip content={`Voice ${s.voice}, ${s.pan}. Bound for life.`}>
          <span className="mt-1 inline-block font-mono text-[10px] text-muted-fg/70">{s.voice}</span>
        </Tip>
      </span>
    </button>
  );
}

function Elapsed({ since }: { since: number | null }) {
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(t);
  }, []);
  if (!since) return null;
  const s = Math.max(0, Math.round((now - since) / 1000));
  return <span className="font-mono tabular-nums">{Math.floor(s / 60)}:{String(s % 60).padStart(2, '0')}</span>;
}

function LiveSession() {
  const g = useGeneration();
  const p = usePlayer();
  const [showLog, setShowLog] = useState(false);
  const live = running(g.phase);

  if (g.phase === 'idle')
    return (
      <Card className="lg:sticky lg:top-20">
        <Empty icon={<Radio />} title="Nothing on air">
          The lines show here as the room speaks. A recap takes a few minutes. A brainstorm is a real room: Opus specialists read the code, 5–15 minutes, then the broadcast is cut and voiced.
        </Empty>
      </Card>
    );

  const openEpisode = async (autoplay: boolean) => {
    if (!g.episode) return;
    go({ view: 'listen', id: g.episode });
    if (autoplay) {
      const { meta, script } = await api.episode(g.episode);
      if (meta.has_audio) p.load(meta, script, { autoplay: true });
    }
  };

  const spoke = new Set(
    [...g.lines.filter((l) => l.kind === 'answer'), ...g.room.filter((t) => t.kind === 'answer')].map((l) => l.speaker),
  );
  return (
    <Card className="flex max-h-[calc(100vh-7rem)] flex-col lg:sticky lg:top-20">
      <div className="flex items-center gap-3 border-b p-4">
        <OnAir live={live} />
        <div className="min-w-0 flex-1">
          <div className="truncate text-sm font-medium">{g.title ?? 'Generation'}</div>
          <div className="text-xs text-muted-fg">
            {g.phase === 'starting' && 'Starting…'}
            {g.phase === 'generating' && (
              <>
                On air · {g.lines.length} lines · <Elapsed since={g.startedAt} />
              </>
            )}
            {g.phase === 'editing' && (
              <>
                Cutting the broadcast from {g.room.filter((t) => t.kind === 'answer').length} answers · <Elapsed since={g.startedAt} />
              </>
            )}
            {g.phase === 'voicing' && (
              <>
                Voicing the script · <Elapsed since={g.startedAt} />
              </>
            )}
            {g.phase === 'done' && (g.status === 'ready' ? 'Ready to play' : 'Script ready (no audio)')}
            {g.phase === 'failed' && <span className="text-danger">Stopped: {g.error ?? 'failed'}</span>}
            {g.phase === 'cancelled' && 'Cancelled; nothing kept'}
          </div>
        </div>
        {live ? (
          <Button size="sm" variant="outline" onClick={g.cancel}>
            <Ban /> Cancel
          </Button>
        ) : (
          <Button size="icon-sm" variant="ghost" onClick={g.dismiss} aria-label="Dismiss">
            <X />
          </Button>
        )}
      </div>
      {spoke.size > 0 && (
        <div className="flex flex-wrap gap-3 border-b px-4 py-2 text-xs">
          {[...spoke].map((sid) => (
            <span key={sid} className="inline-flex items-center gap-1.5" style={{ color: speakerColor(sid) }}>
              <SpeakerDot id={sid} /> {g.lines.find((l) => l.speaker === sid)?.name ?? g.room.find((t) => t.speaker === sid)?.name ?? sid}
            </span>
          ))}
        </div>
      )}
      <div className="min-h-0 flex-1 overflow-y-auto p-3">
        {g.room.length > 0 && (
          <div className={cn(g.lines.length ? 'mb-4 border-b pb-4' : '')}>
            <div className="mb-2 px-1 text-xs font-semibold uppercase tracking-wider text-muted-fg">The room</div>
            <RoomView turns={g.room} names={Object.fromEntries(g.room.map((t) => [t.speaker, t.name ?? t.speaker]))} live />
          </div>
        )}
        {g.lines.length ? (
          <ScriptView lines={g.lines} active={live ? g.lines[g.lines.length - 1]?.i : null} compact />
        ) : g.room.length ? null : (
          <div className="p-6 text-center text-sm text-muted-fg">{live ? 'Waiting for the host to open…' : ''}</div>
        )}
        {showLog && (
          <div className="mt-3 space-y-0.5 rounded-lg bg-muted p-3 font-mono text-[11px] text-muted-fg">
            {g.logs.map((l, k) => (
              <div key={k}>{l}</div>
            ))}
          </div>
        )}
      </div>
      <div className="flex items-center gap-2 border-t p-3">
        <Button size="sm" variant="ghost" onClick={() => setShowLog(!showLog)}>
          {showLog ? 'Hide' : 'Show'} log
        </Button>
        {g.phase === 'done' && g.episode && (
          <div className="ml-auto flex gap-2">
            <Button size="sm" variant="outline" onClick={() => openEpisode(false)}>
              Open
            </Button>
            {g.status === 'ready' && (
              <Button size="sm" onClick={() => openEpisode(true)}>
                Listen now
              </Button>
            )}
          </div>
        )}
      </div>
    </Card>
  );
}
