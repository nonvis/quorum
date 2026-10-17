import { GraduationCap, Lightbulb, Moon, Radio, Sun, FolderKanban, Headphones, Users } from 'lucide-react';
import { useQuery } from '@tanstack/react-query';
import { api } from '@/lib/api';
import { running, useGeneration } from '@/lib/generation';
import { href, type Route } from '@/lib/route';
import type { Theme } from '@/lib/theme';
import { cn } from '@/lib/utils';
import { Badge } from './ui/badge';
import { Button } from './ui/button';
import { Tip } from './ui/tooltip';
import { OnAir } from './bits';

const NAV = [
  { view: 'listen', label: 'Listen', icon: Headphones },
  { view: 'generate', label: 'Generate', icon: Lightbulb },
  { view: 'study', label: 'Study', icon: GraduationCap },
  { view: 'roles', label: 'Roles', icon: Users },
  { view: 'projects', label: 'Projects', icon: FolderKanban },
] as const;

export function TopBar({ route, theme, onTheme }: { route: Route; theme: Theme; onTheme: () => void }) {
  const g = useGeneration();
  const health = useQuery({ queryKey: ['health'], queryFn: api.health, staleTime: 30_000 });
  const live = running(g.phase);
  return (
    <header className="sticky top-0 z-30 border-b bg-bg/90 backdrop-blur">
      <div className="mx-auto flex h-14 max-w-7xl items-center gap-4 px-4">
        <a href={href({ view: 'listen' })} className="flex items-center gap-2 font-semibold tracking-tight">
          <span className="flex size-8 items-center justify-center rounded-lg bg-accent text-accent-fg">
            <Radio className="size-4" />
          </span>
          <span className="hidden sm:inline">Radio Studio</span>
        </a>
        <nav className="flex items-center gap-1">
          {NAV.map(({ view, label, icon: I }) => (
            <a
              key={view}
              href={href({ view } as Route)}
              className={cn(
                'flex items-center gap-2 rounded-lg px-3 py-1.5 text-sm font-medium text-muted-fg transition-colors hover:bg-muted hover:text-fg',
                route.view === view && 'bg-muted text-fg',
              )}
            >
              <I className="size-4" />
              <span className="hidden md:inline">{label}</span>
              {view === 'generate' && live && <OnAir live />}
            </a>
          ))}
        </nav>
        <div className="ml-auto flex items-center gap-2">
          {health.data?.fake && (
            <Tip content="The server runs with --fake: no model calls, a fake voice.">
              <Badge variant="accent">fake model</Badge>
            </Tip>
          )}
          {health.data && !health.data.voice && (
            <Tip content="No voice sidecar here (radio-voice + ffmpeg): episodes come out as scripts only.">
              <Badge variant="outline">no voice</Badge>
            </Tip>
          )}
          {health.isError && <Badge variant="danger">API down</Badge>}
          <Button size="icon-sm" variant="ghost" onClick={onTheme} aria-label="Toggle theme">
            {theme === 'dark' ? <Sun /> : <Moon />}
          </Button>
        </div>
      </div>
    </header>
  );
}
