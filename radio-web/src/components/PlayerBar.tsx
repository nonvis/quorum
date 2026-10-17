import { Pause, Play, RotateCcw, RotateCw, X } from 'lucide-react';
import { activeLine, clock } from '@/lib/format';
import { nextRate, usePlayer } from '@/lib/player';
import { go } from '@/lib/route';
import { speakerColor } from '@/lib/speakers';
import { Button } from './ui/button';
import { Slider } from './ui/slider';
import { KindIcon } from './bits';

export function PlayerBar() {
  const p = usePlayer();
  if (!p.episode) return null;
  const cur = activeLine(p.script?.audio, p.time);
  const line = p.script?.lines.find((l) => l.i === cur);
  return (
    <div className="fixed inset-x-0 bottom-0 z-40 border-t bg-card/95 backdrop-blur supports-[backdrop-filter]:bg-card/80">
      <div className="mx-auto flex max-w-7xl items-center gap-3 px-4 py-2.5">
        <Button size="icon" className="rounded-full" onClick={p.toggle} aria-label={p.playing ? 'Pause' : 'Play'}>
          {p.playing ? <Pause /> : <Play className="translate-x-px" />}
        </Button>
        <div className="hidden items-center sm:flex">
          <Button size="icon-sm" variant="ghost" onClick={() => p.skip(-15)} aria-label="Back 15 seconds">
            <RotateCcw />
          </Button>
          <Button size="icon-sm" variant="ghost" onClick={() => p.skip(15)} aria-label="Forward 15 seconds">
            <RotateCw />
          </Button>
        </div>
        <div className="min-w-0 flex-1">
          <button
            type="button"
            className="flex w-full min-w-0 items-center gap-2 text-left text-sm"
            onClick={() => go({ view: 'listen', id: p.episode!.id })}
          >
            <KindIcon kind={p.episode.kind} className="shrink-0 text-muted-fg" />
            <span className="truncate font-medium">{p.episode.title}</span>
            {line && (
              <span className="hidden shrink-0 text-xs font-semibold md:inline" style={{ color: speakerColor(line.speaker) }}>
                · {line.name ?? line.speaker}
              </span>
            )}
          </button>
          <div className="mt-1.5 flex items-center gap-2">
            <span className="w-10 text-right font-mono text-[11px] tabular-nums text-muted-fg">{clock(p.time)}</span>
            <Slider
              value={[p.time]}
              max={p.duration || 1}
              step={0.5}
              onValueChange={([v]) => p.seek(v)}
              aria-label="Seek"
            />
            <span className="w-10 font-mono text-[11px] tabular-nums text-muted-fg">{clock(p.duration)}</span>
          </div>
        </div>
        <Button size="sm" variant="ghost" className="font-mono tabular-nums" onClick={() => p.setRate(nextRate(p.rate))}>
          {p.rate}×
        </Button>
        <Button size="icon-sm" variant="ghost" onClick={p.close} aria-label="Close player">
          <X />
        </Button>
      </div>
    </div>
  );
}
