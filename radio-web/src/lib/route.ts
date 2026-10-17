import { useEffect, useState } from 'react';

export type Route =
  | { view: 'listen'; id?: string }
  | { view: 'generate' }
  | { view: 'projects' }
  | { view: 'roles'; id?: string };

export function parse(hash: string): Route {
  const [view, id] = hash.replace(/^#\/?/, '').split('/');
  if (view === 'generate') return { view: 'generate' };
  if (view === 'projects') return { view: 'projects' };
  if (view === 'roles') return { view: 'roles', id: id ? decodeURIComponent(id) : undefined };
  return { view: 'listen', id: view === 'listen' && id ? decodeURIComponent(id) : undefined };
}

export function href(r: Route): string {
  if (r.view === 'listen') return r.id ? `#/listen/${encodeURIComponent(r.id)}` : '#/listen';
  if (r.view === 'roles') return r.id ? `#/roles/${encodeURIComponent(r.id)}` : '#/roles';
  return `#/${r.view}`;
}

export function go(r: Route) {
  window.location.hash = href(r);
}

export function useRoute(): Route {
  const [route, setRoute] = useState(() => parse(window.location.hash));
  useEffect(() => {
    const on = () => setRoute(parse(window.location.hash));
    window.addEventListener('hashchange', on);
    return () => window.removeEventListener('hashchange', on);
  }, []);
  return route;
}
