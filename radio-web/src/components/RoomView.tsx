import { useState } from 'react';
import { ArrowRight, ChevronDown } from 'lucide-react';
import { cn } from '@/lib/utils';
import { speakerColor } from '@/lib/speakers';
import type { RoomTurn } from '@/lib/types';
import { Badge } from './ui/badge';

/** A brainstorm's room (QR-31): the host's routing, each specialist's full answer, the close. */
export function RoomView({ turns, names, live = false }: { turns: RoomTurn[]; names: Record<string, string>; live?: boolean }) {
  return (
    <ol className="space-y-2">
      {turns.map((t) =>
        t.kind === 'route' ? (
          <li key={t.n} className="flex items-start gap-2 px-1 text-xs text-muted-fg">
            <span className="font-mono text-[10px] opacity-60">{t.n}</span>
            <span className="inline-flex shrink-0 items-center gap-1 font-medium">
              Host <ArrowRight className="size-3" />
              <span style={{ color: t.to === 'wrap' ? undefined : speakerColor(t.to ?? '') }}>{t.to === 'wrap' ? 'wrap' : names[t.to ?? ''] ?? t.to}</span>
            </span>
            {t.prompt && <span className="min-w-0 italic leading-relaxed">{t.prompt}</span>}
          </li>
        ) : t.kind === 'close' ? (
          <li key={t.n} className="rounded-lg bg-muted/60 p-3 text-sm">
            <div className="mb-1 text-xs font-semibold text-muted-fg">Host closes</div>
            <p className="whitespace-pre-wrap leading-relaxed">{t.text}</p>
          </li>
        ) : (
          <Answer key={t.n} t={t} defaultOpen={!live} />
        ),
      )}
    </ol>
  );
}

function Answer({ t, defaultOpen }: { t: RoomTurn; defaultOpen: boolean }) {
  const [open, setOpen] = useState(defaultOpen);
  const pass = t.kind === 'pass';
  return (
    <li className={cn('rounded-lg border-l-[3px] bg-card px-3 py-2', pass && 'opacity-55')} style={{ borderLeftColor: speakerColor(t.speaker) }}>
      <button type="button" onClick={() => setOpen(!open)} className="flex w-full items-start gap-2 text-left">
        <span className="font-mono text-[10px] text-muted-fg/60">{t.n}</span>
        <span className="shrink-0 text-xs font-semibold" style={{ color: speakerColor(t.speaker) }}>
          {t.name ?? t.speaker}
        </span>
        {pass ? (
          <Badge>pass</Badge>
        ) : (
          <span className="min-w-0 flex-1 text-sm font-medium leading-snug">{t.verdict}</span>
        )}
        {!pass && <ChevronDown className={cn('mt-0.5 size-4 shrink-0 text-muted-fg transition-transform', open && 'rotate-180')} />}
      </button>
      {open && !pass && <p className="mt-2 whitespace-pre-wrap text-sm leading-relaxed text-fg/90">{t.text}</p>}
    </li>
  );
}
