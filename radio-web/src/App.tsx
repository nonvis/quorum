import { useEffect, useState } from 'react';
import { Toaster } from 'sonner';
import { usePlayer } from '@/lib/player';
import { useRoute } from '@/lib/route';
import { applyTheme, initialTheme, type Theme } from '@/lib/theme';
import { cn } from '@/lib/utils';
import { PlayerBar } from '@/components/PlayerBar';
import { TopBar } from '@/components/TopBar';
import { GenerateView } from '@/views/GenerateView';
import { ListenView } from '@/views/ListenView';
import { ProjectsView } from '@/views/ProjectsView';
import { RolesView } from '@/views/RolesView';

export function App() {
  const route = useRoute();
  const player = usePlayer();
  const [theme, setTheme] = useState<Theme>(initialTheme);
  useEffect(() => applyTheme(theme), [theme]);

  return (
    <div className={cn('min-h-full', player.episode && 'pb-24')}>
      <TopBar route={route} theme={theme} onTheme={() => setTheme(theme === 'dark' ? 'light' : 'dark')} />
      {route.view === 'listen' && <ListenView id={route.id} />}
      {route.view === 'generate' && <GenerateView />}
      {route.view === 'projects' && <ProjectsView />}
      {route.view === 'roles' && <RolesView id={route.id} />}
      <PlayerBar />
      <Toaster theme={theme} position="top-right" richColors closeButton />
    </div>
  );
}
