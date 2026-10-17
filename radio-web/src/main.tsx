import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { TooltipProvider } from '@/components/ui/tooltip';
import { DraftProvider } from '@/lib/draft';
import { GenerationProvider } from '@/lib/generation';
import { PlayerProvider } from '@/lib/player';
import { applyTheme, initialTheme } from '@/lib/theme';
import { App } from './App';
import './index.css';

applyTheme(initialTheme());

const qc = new QueryClient({ defaultOptions: { queries: { retry: 1, refetchOnWindowFocus: true } } });

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <QueryClientProvider client={qc}>
      <TooltipProvider>
        <DraftProvider>
          <PlayerProvider>
            <GenerationProvider>
              <App />
            </GenerationProvider>
          </PlayerProvider>
        </DraftProvider>
      </TooltipProvider>
    </QueryClientProvider>
  </StrictMode>,
);
