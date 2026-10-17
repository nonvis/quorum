import { useState } from 'react';
import { ThumbsDown, ThumbsUp } from 'lucide-react';
import { cn } from '@/lib/utils';
import type { Feedback } from '@/lib/types';

/** 👍 / 👎 on a room turn or a broadcast line; a 👎 asks what was off (the tuning loop). */
export function Vote({
  mine,
  onVote,
  onClear,
}: {
  mine?: Feedback;
  onVote: (vote: 'up' | 'down', note?: string) => void;
  onClear: (id: string) => void;
}) {
  const [asking, setAsking] = useState(false);
  const [note, setNote] = useState('');
  const stop = (e: React.SyntheticEvent) => e.stopPropagation();
  if (asking)
    return (
      <span className="flex w-full items-center gap-1.5" onClick={stop}>
        <input
          autoFocus
          value={note}
          onChange={(e) => setNote(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === 'Enter') {
              onVote('down', note);
              setAsking(false);
              setNote('');
            }
            if (e.key === 'Escape') setAsking(false);
          }}
          placeholder="What was off? (Enter)"
          className="h-7 min-w-0 flex-1 rounded-md border bg-card px-2 text-xs focus:outline-none focus:ring-1 focus:ring-ring"
        />
      </span>
    );
  const btn = (v: 'up' | 'down') => {
    const on = mine?.vote === v;
    const I = v === 'up' ? ThumbsUp : ThumbsDown;
    return (
      <button
        type="button"
        aria-label={v === 'up' ? 'Good' : 'Off'}
        title={on && mine?.note ? mine.note : v === 'up' ? 'Good' : 'Off: say why'}
        onClick={(e) => {
          stop(e);
          if (on && mine) onClear(mine.id);
          else if (v === 'up') onVote('up');
          else setAsking(true);
        }}
        className={cn(
          'rounded p-1 transition-colors hover:bg-muted',
          on ? (v === 'up' ? 'text-ok' : 'text-danger') : 'text-muted-fg/50 opacity-0 group-hover:opacity-100',
        )}
      >
        <I className="size-3.5" />
      </button>
    );
  };
  return (
    <span className="inline-flex items-center" onClick={stop}>
      {btn('up')}
      {btn('down')}
    </span>
  );
}
