import { Lightbulb, Radio } from 'lucide-react';
import { cn } from '@/lib/utils';
import { speakerColor } from '@/lib/speakers';
import type { Kind } from '@/lib/types';

export function OnAir({ live = false, className }: { live?: boolean; className?: string }) {
  return (
    <span className={cn('relative inline-flex size-2.5', className)}>
      {live && <span className="absolute inline-flex size-full animate-ping rounded-full bg-accent opacity-60" />}
      <span className={cn('relative inline-flex size-2.5 rounded-full', live ? 'bg-accent' : 'bg-muted-fg/40')} />
    </span>
  );
}

export function SpeakerDot({ id, className }: { id: string; className?: string }) {
  return <span className={cn('inline-block size-2 shrink-0 rounded-full', className)} style={{ background: speakerColor(id) }} />;
}

export function KindIcon({ kind, className }: { kind: Kind; className?: string }) {
  const I = kind === 'brainstorm' ? Lightbulb : Radio;
  return <I className={cn('size-4', className)} aria-label={kind} />;
}

export function Empty({ icon, title, children }: { icon: React.ReactNode; title: string; children?: React.ReactNode }) {
  return (
    <div className="flex flex-col items-center justify-center gap-3 px-6 py-16 text-center">
      <div className="flex size-12 items-center justify-center rounded-full bg-muted text-muted-fg [&_svg]:size-6">{icon}</div>
      <div className="font-medium">{title}</div>
      {children && <div className="max-w-sm text-sm text-muted-fg">{children}</div>}
    </div>
  );
}
