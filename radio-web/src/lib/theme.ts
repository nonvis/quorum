export type Theme = 'light' | 'dark';

export function initialTheme(): Theme {
  try {
    const t = localStorage.getItem('quorum-radio-theme');
    if (t === 'light' || t === 'dark') return t;
  } catch {
    /* no storage */
  }
  return window.matchMedia?.('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
}

export function applyTheme(t: Theme) {
  document.documentElement.dataset.theme = t;
  try {
    localStorage.setItem('quorum-radio-theme', t);
  } catch {
    /* no storage */
  }
}
