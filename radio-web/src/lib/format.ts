import type { AudioTiming, EpisodeMeta } from './types';

/** 0:07, 2:34, 1:02:03 */
export function clock(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) seconds = 0;
  const s = Math.floor(seconds % 60);
  const m = Math.floor(seconds / 60) % 60;
  const h = Math.floor(seconds / 3600);
  const mm = h ? String(m).padStart(2, '0') : String(m);
  return `${h ? `${h}:` : ''}${mm}:${String(s).padStart(2, '0')}`;
}

const KST = 'Asia/Seoul';

/** "Today 14:05", "Yesterday 22:10", "Thu 16 Oct, 09:12" — in KST, the store's clock (QR-7). */
export function when(iso: string, now: Date = new Date()): string {
  const d = new Date(iso);
  const day = (x: Date) => x.toLocaleDateString('en-CA', { timeZone: KST });
  const time = d.toLocaleTimeString('en-GB', { timeZone: KST, hour: '2-digit', minute: '2-digit' });
  if (day(d) === day(now)) return `Today ${time}`;
  if (day(d) === day(new Date(now.getTime() - 86_400_000))) return `Yesterday ${time}`;
  const date = d.toLocaleDateString('en-GB', { timeZone: KST, weekday: 'short', day: 'numeric', month: 'short' });
  return `${date}, ${time}`;
}

/** What happens to an episode if nothing is done: "kept", "expires today", "6 days left". */
export function retention(m: Pick<EpisodeMeta, 'kept' | 'days_left'>): string {
  if (m.kept) return 'kept';
  if (m.days_left === null || m.days_left <= 0) return 'expires today';
  return m.days_left === 1 ? '1 day left' : `${m.days_left} days left`;
}

/** The line on air at time t (the last one that started), or null before the first. */
export function activeLine(audio: AudioTiming | null | undefined, t: number): number | null {
  if (!audio?.lines.length) return null;
  let cur: number | null = null;
  for (const l of audio.lines) {
    if (l.start <= t + 0.05) cur = l.i;
    else break;
  }
  return cur;
}

/** The KST date a range chip resolves to, as YYYY-MM-DD. */
export function kstDate(d: Date = new Date()): string {
  return d.toLocaleDateString('en-CA', { timeZone: KST });
}

/** Grouping for the episode list: Today / Yesterday / This week / Earlier, in KST. */
export function bucket(iso: string, now: Date = new Date()): string {
  const days = Math.round(
    (Date.parse(kstDate(now)) - Date.parse(kstDate(new Date(iso)))) / 86_400_000,
  );
  if (days <= 0) return 'Today';
  if (days === 1) return 'Yesterday';
  if (days < 7) return 'This week';
  return 'Earlier';
}

export function wordCount(s: string): number {
  return s.trim() ? s.trim().split(/\s+/).length : 0;
}
