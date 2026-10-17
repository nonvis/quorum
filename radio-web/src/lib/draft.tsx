import { createContext, useContext, useEffect, useState, type ReactNode } from 'react';
import type { Kind } from './types';

/** The Generate form, kept while you browse (and the question across reloads, per browser). */
export interface Draft {
  kind: Kind;
  project: string | null;
  range: string;
  date: string;
  invite: string[]; // QR-40: product / operator join a recap only when invited
  question: string;
  specialties: string[] | null; // null = the default four
  context: string[];
}

const DEFAULT: Draft = {
  kind: 'recap', project: null, range: 'today', date: '', invite: [],
  question: '', specialties: null, context: [],
};
const KEY = 'quorum-radio-draft';

function stored(): Partial<Draft> {
  try {
    return JSON.parse(localStorage.getItem(KEY) ?? '{}') as Partial<Draft>;
  } catch {
    return {};
  }
}

const Ctx = createContext<{ draft: Draft; patch: (d: Partial<Draft>) => void } | null>(null);

export function DraftProvider({ children }: { children: ReactNode }) {
  const [draft, setDraft] = useState<Draft>(() => ({ ...DEFAULT, ...stored() }));
  useEffect(() => {
    try {
      localStorage.setItem(KEY, JSON.stringify(draft));
    } catch {
      /* private window: the draft just isn't remembered */
    }
  }, [draft]);
  return <Ctx.Provider value={{ draft, patch: (d) => setDraft((p) => ({ ...p, ...d })) }}>{children}</Ctx.Provider>;
}

export function useDraft() {
  const c = useContext(Ctx);
  if (!c) throw new Error('useDraft outside DraftProvider');
  return c;
}
