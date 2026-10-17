import { useEffect, useRef } from 'react';
import { CornerDownRight } from 'lucide-react';
import { cn } from '@/lib/utils';
import { speakerColor } from '@/lib/speakers';
import type { Line, Source } from '@/lib/types';
import { Badge } from './ui/badge';
import { Tip } from './ui/tooltip';

function sourceLabel(s: Source): string {
  if (s.kind === 'commit' || s.kind === 'code') return `${s.kind} · ${s.ref}`;
  if (s.kind === 'evidence' && !s.ref.endsWith('.md') && !s.ref.includes('#')) return `read · ${s.ref}`;
  const ref = s.ref.split('#')[0];
  const stem = ref.split('/').pop()?.replace(/\.md$/, '') ?? ref;
  return `${s.kind} · ${stem}`;
}

interface Props {
  lines: Line[];
  active?: number | null;
  onSeek?: (i: number) => void;
  seekable?: Set<number>;
  follow?: boolean;
  compact?: boolean;
}

export function ScriptView({ lines, active, onSeek, seekable, follow = true, compact = false }: Props) {
  const box = useRef<HTMLOListElement>(null);
  useEffect(() => {
    if (!follow || active === null || active === undefined) return;
    box.current?.querySelector(`[data-line="${active}"]`)?.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
  }, [active, follow]);

  const jump = (i: number) =>
    box.current?.querySelector(`[data-line="${i}"]`)?.scrollIntoView({ block: 'center', behavior: 'smooth' });

  return (
    <ol ref={box} className={cn('space-y-1', compact && 'text-sm')}>
      {lines
        .filter((l) => l.on_air)
        .map((l) => {
          const host = l.speaker === 'host';
          const pass = l.kind === 'pass';
          const can = !!onSeek && (!seekable || seekable.has(l.i));
          return (
            <li
              key={l.i}
              data-line={l.i}
              onClick={can ? () => onSeek?.(l.i) : undefined}
              className={cn(
                'group relative rounded-lg border-l-[3px] px-3 py-2 transition-colors',
                can && 'cursor-pointer hover:bg-muted/60',
                active === l.i && 'bg-accent-soft/70',
                host && 'border-l-transparent',
                pass && 'opacity-55',
              )}
              style={host ? undefined : { borderLeftColor: speakerColor(l.speaker) }}
            >
              <div className="mb-0.5 flex flex-wrap items-center gap-x-2 gap-y-1 text-xs">
                <span className="font-semibold" style={{ color: speakerColor(l.speaker) }}>
                  {l.name ?? l.speaker}
                </span>
                <span className="font-mono text-[10px] text-muted-fg/60">{l.i}</span>
                {l.label === 'inferred' && <Badge variant="outline">guess</Badge>}
                {l.verdict === 'proven' && <Badge variant="ok">proven</Badge>}
                {l.verdict === 'not_proven' && <Badge variant="danger">not proven</Badge>}
                {l.withdrawn && <Badge variant="danger">withdrawn</Badge>}
                {l.about !== null && l.about !== undefined && !host && (
                  <button
                    type="button"
                    onClick={(e) => {
                      e.stopPropagation();
                      jump(l.about!);
                    }}
                    className="inline-flex items-center gap-0.5 text-muted-fg hover:text-fg"
                  >
                    <CornerDownRight className="size-3" /> on line {l.about}
                  </button>
                )}
              </div>
              <p className={cn('leading-relaxed', host && 'text-muted-fg italic', l.withdrawn && 'line-through')}>{l.text}</p>
              {l.source && !compact && (
                <Tip content={l.source.text ? l.source.text.slice(0, 400) : undefined}>
                  <p className="mt-1 truncate font-mono text-[11px] text-muted-fg/80">{sourceLabel(l.source)}</p>
                </Tip>
              )}
            </li>
          );
        })}
    </ol>
  );
}
