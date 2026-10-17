import { useMemo, useState } from 'react';
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { toast } from 'sonner';
import {
  ArrowLeft, ChevronDown, ClipboardCopy, Download, FileText, Lightbulb, MoreHorizontal, Pause, Play, Star, Trash2,
} from 'lucide-react';
import { api } from '@/lib/api';
import { useDraft } from '@/lib/draft';
import { activeLine, clock, retention, when } from '@/lib/format';
import { usePlayer } from '@/lib/player';
import { go } from '@/lib/route';
import { speakerColor } from '@/lib/speakers';
import { cn } from '@/lib/utils';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Card } from '@/components/ui/card';
import { Dialog, DialogClose, DialogContent, DialogHeader } from '@/components/ui/dialog';
import { DropdownMenu, DropdownMenuContent, DropdownMenuItem, DropdownMenuTrigger } from '@/components/ui/dropdown';
import { KindIcon, OnAir, SpeakerDot } from '@/components/bits';
import { ScriptView, type FeedbackCtl } from '@/components/ScriptView';
import { RoomView } from '@/components/RoomView';
import { Tabs, TabsList, TabsTrigger } from '@/components/ui/tabs';

export function EpisodeDetail({ id }: { id: string }) {
  const qc = useQueryClient();
  const p = usePlayer();
  const { patch } = useDraft();
  const [confirm, setConfirm] = useState(false);
  const [showLog, setShowLog] = useState(false);
  const [tab, setTab] = useState<'broadcast' | 'room'>('broadcast');
  const q = useQuery({
    queryKey: ['episode', id],
    queryFn: () => api.episode(id),
    refetchInterval: (query) => {
      const s = query.state.data?.meta.status;
      return s === 'generating' || s === 'editing' || s === 'voicing' ? 2000 : false;
    },
  });
  const keep = useMutation({
    mutationFn: (kept: boolean) => api.keep(id, kept),
    onSuccess: (m) => {
      toast(m.kept ? 'Kept' : 'No longer kept', {
        description: m.kept ? 'It stays until you delete it.' : 'It will be deleted 7 days after it was made.',
      });
      void qc.invalidateQueries({ queryKey: ['episodes'] });
      void qc.invalidateQueries({ queryKey: ['episode', id] });
    },
  });
  const del = useMutation({
    mutationFn: () => api.remove(id),
    onSuccess: () => {
      if (p.episode?.id === id) p.close();
      toast('Deleted');
      void qc.invalidateQueries({ queryKey: ['episodes'] });
      go({ view: 'listen' });
    },
  });

  const fbq = useQuery({ queryKey: ['feedback', id], queryFn: () => api.feedback(id) });
  const fbDone = () => {
    void qc.invalidateQueries({ queryKey: ['feedback', id] });
    void qc.invalidateQueries({ queryKey: ['roles'] });
  };
  const feedback: FeedbackCtl = {
    list: fbq.data ?? [],
    vote: (target, n, vote, note) =>
      api.vote(id, { target, n, vote, note }).then(() => {
        fbDone();
        if (vote === 'down') toast('Noted', { description: 'It shows on that specialty’s Roles page, where you can tune and replay.' });
      }).catch((e) => toast.error('Not saved', { description: String(e) })),
    clear: (fid) => api.unvote(id, fid).then(fbDone),
  };
  const data = q.data;
  const script = data?.script ?? null;
  const meta = data?.meta;
  const isCurrent = p.episode?.id === id;
  const active = isCurrent ? activeLine(script?.audio, p.time) : null;
  const starts = useMemo(() => new Map((script?.audio?.lines ?? []).map((l) => [l.i, l.start])), [script]);
  const speakers = useMemo(() => {
    const m = new Map<string, string>();
    for (const l of script?.lines ?? []) if (l.on_air && l.speaker !== 'host') m.set(l.speaker, l.name ?? l.speaker);
    return [...m.entries()];
  }, [script]);

  if (q.isLoading) return <div className="h-96 animate-pulse rounded-xl bg-muted" />;
  if (q.isError || !meta)
    return (
      <Card className="p-8 text-center text-sm text-muted-fg">
        This episode is gone (deleted, or swept after 7 days unkept).
        <div className="mt-4">
          <Button variant="outline" onClick={() => go({ view: 'listen' })}>
            Back to the list
          </Button>
        </div>
      </Card>
    );

  const playable = meta.has_audio && meta.status === 'ready';
  const busy = meta.status === 'generating' || meta.status === 'editing' || meta.status === 'voicing';
  const play = () => {
    if (isCurrent) p.toggle();
    else p.load(meta, script, { autoplay: true });
  };
  const seekTo = (i: number) => {
    const t = starts.get(i);
    if (t === undefined) return;
    if (isCurrent) p.seek(t);
    else p.load(meta, script, { autoplay: true, at: t });
  };
  const copy = async (what: 'script' | 'room' = 'script') => {
    try {
      await navigator.clipboard.writeText(await (what === 'room' ? api.roomMd(id) : api.scriptMd(id)));
      toast.success(what === 'room' ? 'Room transcript copied' : 'Script copied', { description: 'Paste it into a new Claude Code session.' });
    } catch (e) {
      toast.error('Could not copy', { description: String(e) });
    }
  };
  const brainstormFrom = () => {
    patch({ kind: 'brainstorm', project: meta.project, context: [id], question: '' });
    go({ view: 'generate' });
  };

  return (
    <div className="space-y-5">
      <Button variant="ghost" size="sm" className="-ml-2 lg:hidden" onClick={() => go({ view: 'listen' })}>
        <ArrowLeft /> Episodes
      </Button>
      <Card className="p-5">
        <div className="flex items-start gap-4">
          <div
            className={cn(
              'flex size-12 shrink-0 items-center justify-center rounded-xl',
              meta.kind === 'brainstorm' ? 'bg-[var(--spk-architect)]/15 text-[var(--spk-architect)]' : 'bg-accent-soft text-accent',
            )}
          >
            <KindIcon kind={meta.kind} className="size-6" />
          </div>
          <div className="min-w-0 flex-1">
            <div className="flex flex-wrap items-center gap-2 text-xs text-muted-fg">
              <Badge>{meta.project_name ?? meta.project}</Badge>
              <span>{meta.kind === 'brainstorm' ? 'Brainstorm' : 'Recap'}</span>
              <span>·</span>
              <span>{when(meta.created)}</span>
              {meta.duration ? (
                <>
                  <span>·</span>
                  <span className="tabular-nums">{clock(meta.duration)}</span>
                </>
              ) : null}
              <span>·</span>
              <span className={cn(meta.kept && 'font-medium text-accent')}>{retention(meta)}</span>
            </div>
            <h1 className="mt-1.5 text-xl font-semibold leading-snug tracking-tight">
              {meta.kind === 'brainstorm' ? meta.question ?? meta.title : meta.title}
            </h1>
            {busy && (
              <div className="mt-2 flex items-center gap-2 text-sm text-accent">
                <OnAir live />{' '}
                {meta.status === 'voicing' ? 'Voicing the script…' : meta.status === 'editing' ? 'Cutting the broadcast…' : 'The room is talking…'}
              </div>
            )}
            {meta.status === 'unvoiced' && (
              <p className="mt-2 text-sm text-muted-fg">Script only: no audio was rendered{meta.error ? ` (${meta.error})` : ''}.</p>
            )}
            {meta.status === 'failed' && <p className="mt-2 text-sm text-danger">This generation stopped before it finished.</p>}
          </div>
        </div>
        <div className="mt-5 flex flex-wrap items-center gap-2">
          <Button onClick={play} disabled={!playable}>
            {isCurrent && p.playing ? <Pause /> : <Play />}
            {isCurrent && p.playing ? 'Pause' : isCurrent ? 'Resume' : 'Play'}
          </Button>
          <Button variant="outline" onClick={() => keep.mutate(!meta.kept)} disabled={busy || keep.isPending}>
            <Star className={cn(meta.kept && 'fill-accent text-accent')} />
            {meta.kept ? 'Kept' : 'Keep'}
          </Button>
          <DropdownMenu>
            <DropdownMenuTrigger asChild>
              <Button variant="outline" disabled={!script}>
                <FileText /> Export <ChevronDown className="opacity-60" />
              </Button>
            </DropdownMenuTrigger>
            <DropdownMenuContent>
              {script?.room && (
                <>
                  <DropdownMenuItem onSelect={() => copy('room')}>
                    <ClipboardCopy /> Copy the room (full)
                  </DropdownMenuItem>
                  <DropdownMenuItem asChild>
                    <a href={api.roomUrl(id)} download>
                      <Download /> Download room.md
                    </a>
                  </DropdownMenuItem>
                </>
              )}
              <DropdownMenuItem onSelect={() => copy('script')}>
                <ClipboardCopy /> Copy {script?.room ? 'the broadcast script' : 'script'}
              </DropdownMenuItem>
              <DropdownMenuItem asChild>
                <a href={api.scriptUrl(id)} download>
                  <Download /> Download script.md
                </a>
              </DropdownMenuItem>
            </DropdownMenuContent>
          </DropdownMenu>
          <Button variant="outline" onClick={brainstormFrom} disabled={!script}>
            <Lightbulb /> Brainstorm from this
          </Button>
          <DropdownMenu>
            <DropdownMenuTrigger asChild>
              <Button variant="ghost" size="icon" aria-label="More" className="ml-auto">
                <MoreHorizontal />
              </Button>
            </DropdownMenuTrigger>
            <DropdownMenuContent>
              <DropdownMenuItem onSelect={() => setShowLog((v) => !v)}>
                <FileText /> {showLog ? 'Hide' : 'Show'} the program log
              </DropdownMenuItem>
              <DropdownMenuItem onSelect={() => setConfirm(true)} className="text-danger [&_svg]:text-danger">
                <Trash2 /> Delete…
              </DropdownMenuItem>
            </DropdownMenuContent>
          </DropdownMenu>
        </div>
        {speakers.length > 0 && (
          <div className="mt-4 flex flex-wrap gap-x-4 gap-y-1.5 border-t pt-4 text-xs">
            {speakers.map(([sid, name]) => (
              <span key={sid} className="inline-flex items-center gap-1.5 font-medium" style={{ color: speakerColor(sid) }}>
                <SpeakerDot id={sid} /> {name}
              </span>
            ))}
          </div>
        )}
      </Card>

      {script?.threads && script.threads.length > 0 && (
        <Card className="p-5">
          <h2 className="mb-2 text-xs font-semibold uppercase tracking-wider text-muted-fg">Open threads</h2>
          <ul className="space-y-1.5 text-sm">
            {script.threads.map((t, k) => (
              <li key={k} className="flex gap-2">
                <span className="mt-2 size-1.5 shrink-0 rounded-full bg-accent" />
                {t}
              </li>
            ))}
          </ul>
        </Card>
      )}

      {script?.room && (
        <Tabs value={tab} onValueChange={(v) => setTab(v as 'broadcast' | 'room')}>
          <TabsList>
            <TabsTrigger value="broadcast">Broadcast</TabsTrigger>
            <TabsTrigger value="room">Room · {script.room.turns.filter((t) => t.kind === 'answer').length} answers</TabsTrigger>
          </TabsList>
        </Tabs>
      )}
      {script?.room && tab === 'room' ? (
        <Card className="p-3 sm:p-4">
          <RoomView turns={script.room.turns} names={Object.fromEntries(script.lines.map((l) => [l.speaker, l.name ?? l.speaker]))} feedback={feedback} />
        </Card>
      ) : (
      <Card className="p-3 sm:p-4">
        {script ? (
          <ScriptView
            lines={script.lines}
            active={active}
            onSeek={playable ? seekTo : undefined}
            seekable={new Set(starts.keys())}
            follow={isCurrent && p.playing}
            feedback={feedback}
          />
        ) : (
          <p className="p-6 text-center text-sm text-muted-fg">The script appears when the room has finished.</p>
        )}
      </Card>
      )}

      {showLog && script?.log && (
        <Card className="p-5">
          <h2 className="mb-2 text-xs font-semibold uppercase tracking-wider text-muted-fg">Program log</h2>
          <div className="space-y-0.5 font-mono text-[11px] leading-relaxed text-muted-fg">
            {meta.model && <div>model: {meta.model}</div>}
            {meta.calls !== undefined && <div>calls: {meta.calls} of {meta.cap}</div>}
            {meta.spend && <div>{meta.spend}</div>}
            {meta.timings && (
              <div>
                time: {Object.entries(meta.timings).map(([k, v]) => `${k} ${Math.round(v)} s`).join(' · ')}
              </div>
            )}
            {script.log.map((l, k) => (
              <div key={k}>{l}</div>
            ))}
          </div>
        </Card>
      )}

      <Dialog open={confirm} onOpenChange={setConfirm}>
        <DialogContent>
          <DialogHeader title="Delete this episode?" description="The script and the audio go now; this can’t be undone." />
          <div className="flex justify-end gap-2">
            <DialogClose asChild>
              <Button variant="outline">Cancel</Button>
            </DialogClose>
            <Button variant="danger" onClick={() => del.mutate()} disabled={del.isPending}>
              <Trash2 /> Delete
            </Button>
          </div>
        </DialogContent>
      </Dialog>
    </div>
  );
}
