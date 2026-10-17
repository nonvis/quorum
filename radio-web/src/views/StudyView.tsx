import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { toast } from 'sonner';
import {
  AlertTriangle, BookOpen, CheckCircle2, FileCode2, GitBranch, GraduationCap, History, Loader2, Plus, Send, Square, Trash2,
} from 'lucide-react';
import { api, ApiError } from '@/lib/api';
import { kstDate, when } from '@/lib/format';
import { go, href } from '@/lib/route';
import type { StudyClose, StudyEvent, StudyTool, StudyTrackSummary, StudyTrackView, StudyTurn } from '@/lib/types';
import { cn } from '@/lib/utils';
import { Prose, type Cite } from '@/components/Prose';
import { Empty } from '@/components/bits';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Card } from '@/components/ui/card';
import { Dialog, DialogContent, DialogHeader } from '@/components/ui/dialog';
import { Input, Label, Textarea } from '@/components/ui/input';
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs';

const usd = (n: number) => `$${(n ?? 0).toFixed(2)}`;
const errText = (e: unknown) => (e instanceof ApiError ? e.message : String(e));

/** Study (QR-41…47): learn one repo ~30 minutes a day; each day one tutor session, its close the memory. */
export function StudyView({ id }: { id?: string }) {
  const tracks = useQuery({ queryKey: ['study-tracks'], queryFn: api.tracks });
  const [adding, setAdding] = useState(false);
  const list = tracks.data ?? [];
  const active = list.filter((t) => t.status === 'active');
  const finished = list.filter((t) => t.status === 'finished');

  useEffect(() => {
    if (!id && active[0]) go({ view: 'study', id: active[0].id });
  }, [id, active]);

  return (
    <div className="mx-auto grid max-w-7xl gap-6 px-4 py-6 lg:grid-cols-[280px_minmax(0,1fr)]">
      <aside className={cn('min-w-0 space-y-4', id && 'hidden lg:block')}>
        <Button className="w-full" onClick={() => setAdding(true)}>
          <Plus /> New track
        </Button>
        {tracks.isLoading ? (
          <div className="h-20 animate-pulse rounded-xl bg-muted" />
        ) : (
          <>
            <TrackList title="Studying" tracks={active} current={id} />
            {finished.length > 0 && <TrackList title="Finished" tracks={finished} current={id} />}
          </>
        )}
      </aside>
      <main className="min-w-0">
        {id ? (
          <TrackPage key={id} id={id} />
        ) : (
          <Card>
            <Empty icon={<GraduationCap />} title="No study track yet">
              A track is one repo and a goal. Each day is one tutor session; ending it saves where you left off, and the
              next day picks up from that summary.
              <div className="mt-4">
                <Button onClick={() => setAdding(true)}>New track</Button>
              </div>
            </Empty>
          </Card>
        )}
      </main>
      <NewTrackDialog open={adding} onOpenChange={setAdding} />
    </div>
  );
}

function TrackList({ title, tracks, current }: { title: string; tracks: StudyTrackSummary[]; current?: string }) {
  if (!tracks.length) return null;
  return (
    <section className="space-y-2">
      <h3 className="px-1 text-xs font-semibold uppercase tracking-wider text-muted-fg">{title}</h3>
      {tracks.map((t) => (
        <a
          key={t.id}
          href={href({ view: 'study', id: t.id })}
          className={cn(
            'block rounded-xl border bg-card p-3 transition-colors hover:bg-muted/60',
            current === t.id && 'border-accent/60 ring-1 ring-accent/30',
          )}
        >
          <div className="flex items-center gap-2">
            <span className="truncate font-medium">{t.id}</span>
            {t.open_day && <Badge variant="accent">open</Badge>}
          </div>
          <div className="mt-0.5 line-clamp-2 text-xs text-muted-fg">{t.goal}</div>
          <div className="mt-1.5 flex gap-3 text-[11px] text-muted-fg">
            <span>{t.days} day{t.days === 1 ? '' : 's'}</span>
            <span>{usd(t.cost)}</span>
            {t.last && <span>last {t.last}</span>}
          </div>
        </a>
      ))}
    </section>
  );
}

interface Live {
  job: string;
  kind: 'say' | 'start' | 'end';
  pending: string | null;
  tools: StudyTool[];
}

function TrackPage({ id }: { id: string }) {
  const qc = useQueryClient();
  const view = useQuery({ queryKey: ['study', id], queryFn: () => api.track(id), retry: false });
  const today = kstDate();
  const v = view.data;
  const days = v?.days ?? [];
  const todayDay = days.find((d) => d.date === today);
  const staleOpen = v?.open_day && v.open_day !== today ? v.open_day : null;
  const [picked, setPicked] = useState<string | null>(null);
  const date = picked ?? staleOpen ?? (todayDay ? today : null);
  const day = useQuery({ queryKey: ['study-day', id, date], queryFn: () => api.studyDay(id, date!), enabled: !!date && days.some((d) => d.date === date) });
  const [live, setLive] = useState<Live | null>(null);
  const [tab, setTab] = useState('memory');
  const [cite, setCite] = useState<Cite | null>(null);
  const [finishing, setFinishing] = useState<'confirm' | 'delete' | null>(null);
  const es = useRef<EventSource | null>(null);

  const refresh = useCallback(() => {
    void qc.invalidateQueries({ queryKey: ['study', id] });
    void qc.invalidateQueries({ queryKey: ['study-day', id] });
    void qc.invalidateQueries({ queryKey: ['study-tracks'] });
  }, [qc, id]);

  const attach = useCallback(
    (job: string, kind: Live['kind'], pending: string | null) => {
      es.current?.close();
      setLive({ job, kind, pending, tools: [] });
      const src = new EventSource(`/api/jobs/${job}/events`);
      es.current = src;
      src.onmessage = (m) => {
        const ev = JSON.parse(m.data) as StudyEvent;
        if (ev.event === 'tool') setLive((p) => (p ? { ...p, tools: [...p.tools, { name: ev.name, target: ev.target }] } : p));
        if (ev.event === 'turn') {
          void qc.invalidateQueries({ queryKey: ['study', id] });      // a new day appears in the list
          void qc.invalidateQueries({ queryKey: ['study-day', id] });
          if (ev.turn.who === 'tutor') setLive((p) => (p ? { ...p, pending: null, tools: [] } : p));
        }
        if (ev.event === 'closed') toast.success('Saved where you left off', { description: 'Tomorrow starts from this summary.' });
        if (ev.event === 'error') toast.error('The tutor stopped', { description: ev.message });
        if (ev.event === 'exit') {
          src.close();
          setLive(null);
          refresh();
        }
      };
      src.onerror = () => {
        if (src.readyState === EventSource.CLOSED) {
          setLive(null);
          refresh();
        }
      };
    },
    [id, qc, refresh],
  );

  useEffect(() => {
    if (v?.running && !live) attach(v.running.id, 'say', null);
  }, [v?.running, live, attach]);
  useEffect(() => () => es.current?.close(), []);

  const run = async (kind: Live['kind'], message?: string) => {
    try {
      const { job } =
        kind === 'say' ? await api.studySay(id, message!) : kind === 'start' ? await api.studyStart(id) : await api.studyEnd(id);
      setPicked(null);
      attach(job, kind, message ?? null);
    } catch (e) {
      toast.error(kind === 'end' ? 'Could not end the session' : 'Could not send', { description: errText(e) });
    }
  };

  const openCite = (c: Cite) => {
    setCite(c);
    setTab('code');
  };

  if (view.isError) return <Card><Empty icon={<AlertTriangle />} title="No such track">{errText(view.error)}</Empty></Card>;
  if (!v) return <div className="h-64 animate-pulse rounded-xl bg-muted" />;

  const t = v.track;
  const isActive = t.status === 'active';
  const viewingToday = !date || date === today;
  const dayOpen = day.data?.status === 'open';
  const busy = !!live;
  const canEnd = isActive && !busy && !!v.open_day && days.some((d) => d.date === v.open_day);
  const dayN = todayDay?.day_n ?? days.length + (todayDay ? 0 : 1);

  return (
    <div className="space-y-4">
      <header className="flex flex-wrap items-start gap-3">
        <div className="min-w-0 flex-1">
          <div className="flex flex-wrap items-center gap-2">
            <h1 className="text-xl font-semibold tracking-tight">{t.goal}</h1>
            {!isActive && <Badge>finished</Badge>}
          </div>
          <div className="mt-1 flex flex-wrap items-center gap-x-4 gap-y-1 text-xs text-muted-fg">
            <span className="font-mono">{t.repo}{t.subtree ? `/${t.subtree}` : ''}</span>
            {v.head?.ref && (
              <span className="inline-flex items-center gap-1"><GitBranch className="size-3" />{v.head.ref} {v.head.sha?.slice(0, 8)}</span>
            )}
            <span>{isActive ? `day ${dayN}` : `${days.length} days`}</span>
            <span>{usd(days.reduce((a, d) => a + (d.cost ?? 0), 0))} so far</span>
          </div>
        </div>
        <div className="flex items-center gap-2">
          {canEnd && (
            <Button variant="outline" onClick={() => void run('end')}>
              <Square /> End session
            </Button>
          )}
          {isActive ? (
            <Button variant="ghost" disabled={busy} onClick={() => (v.open_day && days.find((d) => d.date === v.open_day) ? toast.info('End the session first', { description: 'Its close is the memory.' }) : setFinishing('confirm'))}>
              <CheckCircle2 /> Finish track
            </Button>
          ) : (
            <Button variant="ghost" onClick={() => setFinishing('delete')}>
              <Trash2 /> Delete track
            </Button>
          )}
        </div>
      </header>

      {staleOpen && isActive && (
        <div className="flex flex-wrap items-center gap-3 rounded-xl border border-accent/40 bg-accent-soft px-4 py-3 text-sm">
          <AlertTriangle className="size-4 text-accent" />
          <span className="flex-1">The session of {staleOpen} was never ended. End it to save its memory before today’s starts.</span>
          <Button size="sm" disabled={busy} onClick={() => void run('end')}>End it</Button>
        </div>
      )}

      <div className="grid gap-4 xl:grid-cols-[minmax(0,1fr)_380px]">
        <Card className="flex min-h-[60vh] flex-col">
          <div className="flex items-center gap-2 border-b px-4 py-2.5 text-sm">
            <BookOpen className="size-4 text-muted-fg" />
            <span className="font-medium">{date ? (date === today ? 'Today' : date) : 'Today'}</span>
            {day.data && <Badge variant={dayOpen ? 'accent' : 'default'}>{dayOpen ? 'open' : 'ended'}</Badge>}
            {day.data && <span className="text-xs text-muted-fg">{usd(day.data.cost)}</span>}
            {!viewingToday && (
              <Button size="sm" variant="ghost" className="ml-auto" onClick={() => setPicked(today)}>Back to today</Button>
            )}
          </div>
          <Chat
            turns={day.data?.turns ?? []}
            close={day.data?.status === 'closed' ? day.data.close : null}
            live={live}
            onCite={openCite}
            empty={
              isActive && viewingToday && !todayDay && !staleOpen ? (
                <Empty icon={<GraduationCap />} title={days.length ? `Day ${dayN}` : 'Day 1'}>
                  {days.length
                    ? 'A new session, loaded with the summary of what you covered. Start it, or ask your first question below.'
                    : 'The tutor starts from nothing: a map of the code and three places to begin. Or ask your own first question below.'}
                  <div className="mt-4">
                    <Button disabled={busy} onClick={() => void run('start')}>Start today’s session</Button>
                  </div>
                </Empty>
              ) : null
            }
          />
          {isActive && viewingToday && !staleOpen && <Composer busy={busy} onSend={(m) => void run('say', m)} />}
        </Card>

        <Card className="min-w-0 self-start xl:sticky xl:top-20">
          <Tabs value={tab} onValueChange={setTab}>
            <div className="border-b px-3 py-2">
              <TabsList>
                <TabsTrigger value="memory"><BookOpen /> Memory</TabsTrigger>
                <TabsTrigger value="code"><FileCode2 /> Code</TabsTrigger>
                <TabsTrigger value="days"><History /> Days</TabsTrigger>
              </TabsList>
            </div>
            <TabsContent value="memory" className="p-4">
              <Memory v={v} onOpen={(path) => openCite({ path, line: 1, end: null })} />
            </TabsContent>
            <TabsContent value="code" className="p-0">
              {cite ? <Excerpt track={id} cite={cite} /> : <p className="p-4 text-sm text-muted-fg">Click a highlighted <code className="font-mono">path:line</code> in an answer, or a covered file, to read it here.</p>}
            </TabsContent>
            <TabsContent value="days" className="p-2">
              {days.length === 0 ? (
                <p className="p-2 text-sm text-muted-fg">No days yet.</p>
              ) : (
                <ol className="space-y-1">
                  {[...days].reverse().map((d) => (
                    <li key={d.date}>
                      <button
                        type="button"
                        onClick={() => setPicked(d.date)}
                        className={cn('w-full rounded-lg px-3 py-2 text-left hover:bg-muted', date === d.date && 'bg-muted')}
                      >
                        <div className="flex items-center gap-2 text-sm">
                          <span className="font-medium">{d.date}</span>
                          {d.status === 'open' && <Badge variant="accent">open</Badge>}
                          <span className="ml-auto text-xs text-muted-fg">{d.turns} turns · {usd(d.cost)}</span>
                        </div>
                        {d.close && <div className="mt-0.5 line-clamp-2 text-xs text-muted-fg">{d.close.left_off}</div>}
                      </button>
                    </li>
                  ))}
                </ol>
              )}
            </TabsContent>
          </Tabs>
        </Card>
      </div>

      <FinishDialog id={id} step={finishing} setStep={setFinishing} days={days.length} onDone={refresh} />
    </div>
  );
}

function Chat({
  turns, close, live, onCite, empty,
}: { turns: StudyTurn[]; close: StudyClose | null; live: Live | null; onCite: (c: Cite) => void; empty: React.ReactNode }) {
  const end = useRef<HTMLDivElement>(null);
  const shown = live?.pending && !turns.some((t) => t.who === 'sang' && t.text === live.pending && t.n === turns.length - 1) ? live.pending : null;
  useEffect(() => {
    end.current?.scrollIntoView({ behavior: 'smooth', block: 'end' });   // newer Chrome returns a promise: never the effect's cleanup
  }, [turns.length, live?.tools.length, shown]);
  if (!turns.length && !live && empty) return <div className="flex flex-1 items-center justify-center">{empty}</div>;
  return (
    <div className="flex-1 space-y-4 overflow-y-auto px-4 py-4">
      {turns.map((t) => (t.who === 'sang' ? <Mine key={t.n} text={t.text} /> : <TutorTurn key={t.n} t={t} onCite={onCite} />))}
      {shown && <Mine text={shown} />}
      {live && live.kind !== 'end' && (
        <div className="rounded-lg border-l-[3px] border-accent/60 bg-muted/40 px-3 py-2 text-xs text-muted-fg">
          <div className="flex items-center gap-2 font-medium"><Loader2 className="size-3.5 animate-spin" /> The tutor is reading…</div>
          {live.tools.length > 0 && (
            <ul className="mt-1.5 space-y-0.5 font-mono text-[11px]">
              {live.tools.slice(-8).map((x, i) => <li key={i} className="truncate">{x.name} {x.target}</li>)}
            </ul>
          )}
        </div>
      )}
      {live?.kind === 'end' && (
        <div className="flex items-center gap-2 rounded-lg bg-muted/40 px-3 py-2 text-xs text-muted-fg">
          <Loader2 className="size-3.5 animate-spin" /> Writing where you left off…
        </div>
      )}
      {close && <CloseCard close={close} />}
      <div ref={end} />
    </div>
  );
}

function Mine({ text }: { text: string }) {
  return (
    <div className="flex justify-end">
      <div className="max-w-[85%] whitespace-pre-wrap rounded-2xl rounded-br-sm bg-accent-soft px-4 py-2 text-sm">{text}</div>
    </div>
  );
}

function TutorTurn({ t, onCite }: { t: StudyTurn; onCite: (c: Cite) => void }) {
  const valid = useMemo(() => new Map((t.citations ?? []).map((c) => [`${c.path}:${c.line}`, c.resolved])), [t.citations]);
  const [tools, setTools] = useState(false);
  return (
    <div className="space-y-1.5">
      <Prose text={t.text} valid={valid} onCite={onCite} />
      <div className="flex flex-wrap items-center gap-3 text-[11px] text-muted-fg">
        {t.tools.length > 0 && (
          <button type="button" className="hover:text-fg" onClick={() => setTools(!tools)}>
            read {t.tools.length} {t.tools.length === 1 ? 'thing' : 'things'}
          </button>
        )}
        {t.secs != null && <span>{Math.round(t.secs)} s</span>}
        <span>{usd(t.cost)}</span>
        <span>{when(t.at)}</span>
      </div>
      {tools && (
        <ul className="rounded-lg bg-muted/50 px-3 py-1.5 font-mono text-[11px] text-muted-fg">
          {t.tools.map((x, i) => <li key={i} className="truncate">{x.name} {x.target}</li>)}
        </ul>
      )}
    </div>
  );
}

function CloseCard({ close }: { close: StudyClose }) {
  return (
    <div className="rounded-xl border border-ok/40 bg-ok/5 p-4 text-sm">
      <div className="mb-2 flex items-center gap-2 font-semibold"><CheckCircle2 className="size-4 text-ok" /> Saved for tomorrow</div>
      <Field label="Left off">{close.left_off}</Field>
      {close.covered.length > 0 && <Field label="Covered today"><ul className="list-disc pl-5">{close.covered.map((c) => <li key={c.path}><code className="font-mono text-xs">{c.path}</code> — {c.what}</li>)}</ul></Field>}
      {close.unclear.length > 0 && <Field label="Still unclear"><ul className="list-disc pl-5">{close.unclear.map((u) => <li key={u}>{u}</li>)}</ul></Field>}
      {close.next.length > 0 && <Field label="Next"><ul className="list-disc pl-5">{close.next.map((u) => <li key={u}>{u}</li>)}</ul></Field>}
    </div>
  );
}

function Field({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="mt-2 first:mt-0">
      <div className="text-[11px] font-semibold uppercase tracking-wide text-muted-fg">{label}</div>
      <div className="mt-0.5 leading-relaxed">{children}</div>
    </div>
  );
}

function Composer({ busy, onSend }: { busy: boolean; onSend: (m: string) => void }) {
  const [text, setText] = useState('');
  const send = () => {
    const m = text.trim();
    if (!m || busy) return;
    onSend(m);
    setText('');
  };
  return (
    <div className="border-t p-3">
      <div className="flex items-end gap-2">
        <Textarea
          value={text}
          rows={2}
          placeholder={busy ? 'The tutor is answering…' : 'Ask about the code — Enter sends, Shift+Enter for a new line'}
          onChange={(e) => setText(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === 'Enter' && !e.shiftKey && !e.nativeEvent.isComposing) {
              e.preventDefault();
              send();
            }
          }}
          className="max-h-48 min-h-[44px] resize-y"
        />
        <Button size="icon" disabled={busy || !text.trim()} onClick={send} aria-label="Send">
          {busy ? <Loader2 className="animate-spin" /> : <Send />}
        </Button>
      </div>
    </div>
  );
}

function Memory({ v, onOpen }: { v: StudyTrackView; onOpen: (path: string) => void }) {
  const st = v.track.state;
  if (!st.left_off && !st.covered.length)
    return <p className="text-sm text-muted-fg">Nothing saved yet. Ending a session writes where you left off; the next day starts from it.</p>;
  return (
    <div className="space-y-3 text-sm">
      <Field label="Left off">{st.left_off}</Field>
      {v.changed.length > 0 && (
        <div className="rounded-lg border border-accent/40 bg-accent-soft px-3 py-2 text-xs">
          <div className="font-semibold">Changed since you read them</div>
          <ul className="mt-1 font-mono">{v.changed.map((p) => <li key={p}>{p}</li>)}</ul>
        </div>
      )}
      <Field label={`Covered (${st.covered.length})`}>
        <ul className="space-y-1">
          {st.covered.map((c) => (
            <li key={c.path}>
              <button type="button" className="font-mono text-xs text-accent hover:underline" onClick={() => onOpen(c.path)}>{c.path}</button>
              <span className="text-muted-fg"> — {c.what}</span>
            </li>
          ))}
        </ul>
      </Field>
      {st.unclear.length > 0 && <Field label="Still unclear"><ul className="list-disc pl-5">{st.unclear.map((u) => <li key={u}>{u}</li>)}</ul></Field>}
      {st.next.length > 0 && <Field label="Next"><ul className="list-disc pl-5">{st.next.map((u) => <li key={u}>{u}</li>)}</ul></Field>}
    </div>
  );
}

function Excerpt({ track, cite }: { track: string; cite: Cite }) {
  const q = useQuery({
    queryKey: ['study-file', track, cite.path, cite.line, cite.end],
    queryFn: () => api.studyFile(track, cite.path, cite.line, cite.end),
    retry: false,
  });
  const hit = useRef<HTMLDivElement>(null);
  useEffect(() => {
    hit.current?.scrollIntoView({ block: 'center' });
  }, [q.data]);
  if (q.isError) return <p className="p-4 text-sm text-danger">{errText(q.error)}</p>;
  if (!q.data) return <div className="m-4 h-40 animate-pulse rounded-lg bg-muted" />;
  const e = q.data;
  return (
    <div>
      <div className="flex items-center gap-2 border-b px-3 py-2 font-mono text-xs">
        <span className="truncate">{e.path}</span>
        <span className="ml-auto shrink-0 text-muted-fg">{e.start}–{e.end} of {e.total}</span>
      </div>
      <div className="max-h-[65vh] overflow-auto py-2 font-mono text-[11.5px] leading-[1.55]">
        {e.lines.map(([n, s]) => {
          const on = n >= e.line && n <= e.line_end;
          return (
            <div key={n} ref={n === e.line ? hit : undefined} className={cn('flex whitespace-pre px-3', on && 'bg-accent-soft')}>
              <span className="mr-3 inline-block w-9 shrink-0 select-none text-right text-muted-fg/60">{n}</span>
              <span>{s || ' '}</span>
            </div>
          );
        })}
      </div>
    </div>
  );
}

function FinishDialog({
  id, step, setStep, days, onDone,
}: { id: string; step: 'confirm' | 'delete' | null; setStep: (s: 'confirm' | 'delete' | null) => void; days: number; onDone: () => void }) {
  const qc = useQueryClient();
  const finish = useMutation({
    mutationFn: () => api.studyFinish(id),
    onSuccess: () => {
      onDone();
      setStep('delete');
    },
    onError: (e) => toast.error('Not finished', { description: errText(e) }),
  });
  const del = useMutation({
    mutationFn: () => api.studyDelete(id),
    onSuccess: () => {
      setStep(null);
      qc.removeQueries({ queryKey: ['study', id] });
      void qc.invalidateQueries({ queryKey: ['study-tracks'] });
      toast.success(`Track ${id} deleted`);
      go({ view: 'study' });
    },
    onError: (e) => toast.error('Not deleted', { description: errText(e) }),
  });
  return (
    <Dialog open={!!step} onOpenChange={(o) => !o && setStep(null)}>
      <DialogContent>
        {step === 'confirm' ? (
          <>
            <DialogHeader title="Finish this track?" description="It leaves the Studying list and takes no more sessions." />
            <div className="flex justify-end gap-2">
              <Button variant="ghost" onClick={() => setStep(null)}>Not yet</Button>
              <Button disabled={finish.isPending} onClick={() => finish.mutate()}>Finish</Button>
            </div>
          </>
        ) : (
          <>
            <DialogHeader
              title="Delete the whole track?"
              description={`Every day's transcript (${days}), the summary and the track itself are removed from this machine. Nothing in the repo is touched.`}
            />
            <div className="flex justify-end gap-2">
              <Button variant="ghost" onClick={() => setStep(null)}>Keep it</Button>
              <Button variant="danger" disabled={del.isPending} onClick={() => del.mutate()}><Trash2 /> Delete</Button>
            </div>
          </>
        )}
      </DialogContent>
    </Dialog>
  );
}

function slug(s: string) {
  return s.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 40);
}

function NewTrackDialog({ open, onOpenChange }: { open: boolean; onOpenChange: (v: boolean) => void }) {
  const qc = useQueryClient();
  const cands = useQuery({ queryKey: ['study-candidates'], queryFn: api.studyCandidates, enabled: open });
  const projects = useQuery({ queryKey: ['projects'], queryFn: api.projects, enabled: open });
  const [filter, setFilter] = useState('');
  const [repo, setRepo] = useState<string | null>(null);
  const [subtree, setSubtree] = useState('');
  const [tid, setTid] = useState('');
  const [project, setProject] = useState('');
  const [goal, setGoal] = useState('');

  useEffect(() => {
    if (!open) {
      setFilter(''); setRepo(null); setSubtree(''); setTid(''); setGoal('');
    }
  }, [open]);
  useEffect(() => {
    if (repo) setTid(slug([repo.split('/').pop(), subtree].filter(Boolean).join('-')));
  }, [repo, subtree]);
  useEffect(() => {
    if (!project && projects.data?.[0]) setProject(projects.data.find((p) => p.active)?.id ?? projects.data[0].id);
  }, [projects.data, project]);

  const create = useMutation({
    mutationFn: () => api.newTrack({ id: tid, project, repo: repo!, subtree: subtree.trim() || undefined, goal }),
    onSuccess: (t) => {
      void qc.invalidateQueries({ queryKey: ['study-tracks'] });
      onOpenChange(false);
      go({ view: 'study', id: t.id });
    },
    onError: (e) => toast.error('Not created', { description: errText(e) }),
  });

  const shown = (cands.data ?? []).filter((c) => !filter || c.path.toLowerCase().includes(filter.toLowerCase()));
  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="max-w-xl">
        <DialogHeader
          title="New study track"
          description="One repo and a goal. The tutor reads it (read-only, never git); nothing about it is indexed or committed."
        />
        <div className="space-y-1.5">
          <Label>Repo</Label>
          <Input placeholder="Filter…" value={filter} onChange={(e) => setFilter(e.target.value)} />
          <div className="max-h-52 overflow-y-auto rounded-lg border">
            {cands.isLoading && <div className="p-3 text-sm text-muted-fg">Looking for checkouts…</div>}
            {shown.map((c) => (
              <button
                key={c.path}
                type="button"
                onClick={() => setRepo(c.path)}
                className={cn('flex w-full items-center gap-2 px-3 py-1.5 text-left text-sm hover:bg-muted', repo === c.path && 'bg-accent-soft')}
              >
                <span className="font-medium">{c.name}</span>
                <span className="truncate font-mono text-[11px] text-muted-fg">{c.path}</span>
                {c.head?.ref && <span className="ml-auto shrink-0 text-[11px] text-muted-fg">{c.head.ref}</span>}
              </button>
            ))}
          </div>
        </div>
        <div className="grid gap-3 sm:grid-cols-2">
          <div className="space-y-1.5">
            <Label>Focus (optional)</Label>
            <Input placeholder="e.g. listener" value={subtree} onChange={(e) => setSubtree(e.target.value)} />
          </div>
          <div className="space-y-1.5">
            <Label>Track id</Label>
            <Input value={tid} onChange={(e) => setTid(slug(e.target.value))} />
          </div>
        </div>
        <div className="space-y-1.5">
          <Label>Project</Label>
          <select value={project} onChange={(e) => setProject(e.target.value)} className="flex h-9 w-full rounded-lg border bg-card px-3 text-sm">
            {(projects.data ?? []).map((p) => <option key={p.id} value={p.id}>{p.name}</option>)}
          </select>
        </div>
        <div className="space-y-1.5">
          <Label>Goal</Label>
          <Input placeholder="e.g. own the V2 listener" value={goal} onChange={(e) => setGoal(e.target.value)} />
        </div>
        <div className="flex justify-end gap-2">
          <Button variant="ghost" onClick={() => onOpenChange(false)}>Cancel</Button>
          <Button disabled={!repo || !tid || !goal.trim() || !project || create.isPending} onClick={() => create.mutate()}>Create track</Button>
        </div>
      </DialogContent>
    </Dialog>
  );
}
