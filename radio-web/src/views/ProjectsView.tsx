import { useEffect, useState } from 'react';
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { toast } from 'sonner';
import { AlertTriangle, CheckCircle2, FolderKanban, GitBranch, Pencil, Plus, X, XCircle } from 'lucide-react';
import { AddProjectDialog } from './AddProjectDialog';
import { api, ApiError } from '@/lib/api';
import type { Project, ProjectCheck, SaveResult } from '@/lib/types';
import { cn } from '@/lib/utils';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Card } from '@/components/ui/card';
import { Dialog, DialogContent, DialogHeader } from '@/components/ui/dialog';
import { Input, Label } from '@/components/ui/input';
import { Switch } from '@/components/ui/switch';
import { Empty } from '@/components/bits';

export function saved(r: SaveResult, what: string) {
  if (r.pushed) toast.success(what, { description: `Committed ${r.committed} to the vault and pushed.` });
  else if (r.committed) toast.warning(what, { description: `Committed ${r.committed}; the push failed: ${r.push_error}` });
  else toast.warning(what, { description: r.push_error ?? 'Saved, not committed.' });
}

export function ProjectsView() {
  const qc = useQueryClient();
  const q = useQuery({ queryKey: ['projects'], queryFn: api.projects });
  const [editing, setEditing] = useState<Project | 'new' | null>(null);
  const [adding, setAdding] = useState(false);
  const [adjust, setAdjust] = useState<typeof EMPTY | undefined>(undefined);
  const toggle = useMutation({
    mutationFn: ({ id, active }: { id: string; active: boolean }) => api.setActive(id, active),
    onSuccess: (r, v) => {
      saved(r, `${v.id} ${v.active ? 'activated' : 'deactivated'}`);
      void qc.invalidateQueries({ queryKey: ['projects'] });
    },
    onError: (e) => toast.error('Not saved', { description: String(e) }),
  });
  const list = [...(q.data ?? [])].sort((a, b) => Number(b.active) - Number(a.active) || a.name.localeCompare(b.name));

  return (
    <div className="mx-auto max-w-5xl space-y-5 px-4 py-6">
      <div className="flex flex-wrap items-end justify-between gap-3">
        <div>
          <h1 className="text-xl font-semibold tracking-tight">Projects</h1>
          <p className="text-sm text-muted-fg">
            What Radio can talk about. Saved in the vault (<code className="font-mono text-xs">06_Metadata/quorum-projects.json</code>);
            each save is committed and pushed.
          </p>
        </div>
        <Button onClick={() => setAdding(true)}>
          <Plus /> Add project
        </Button>
      </div>
      {q.isLoading ? (
        <div className="h-48 animate-pulse rounded-xl bg-muted" />
      ) : list.length === 0 ? (
        <Card>
          <Empty icon={<FolderKanban />} title="No projects" />
        </Card>
      ) : (
        <div className="grid gap-3 md:grid-cols-2">
          {list.map((p) => (
            <Card key={p.id} className={cn('p-4 transition-opacity', !p.active && 'opacity-60')}>
              <div className="flex items-start gap-3">
                <div className="min-w-0 flex-1">
                  <div className="flex items-center gap-2">
                    <span className="font-medium">{p.name}</span>
                    <code className="font-mono text-[11px] text-muted-fg">{p.id}</code>
                    {!p.active && <Badge>inactive</Badge>}
                  </div>
                </div>
                <Button size="icon-sm" variant="ghost" onClick={() => setEditing(p)} aria-label={`Edit ${p.name}`}>
                  <Pencil />
                </Button>
                <Switch
                  checked={p.active}
                  disabled={toggle.isPending}
                  onCheckedChange={(v) => toggle.mutate({ id: p.id, active: v })}
                  aria-label={p.active ? 'Deactivate' : 'Activate'}
                />
              </div>
              <dl className="mt-3 space-y-1.5 text-xs">
                <Row k="Briefing" v={p.notes} />
                <Row k="Folders" v={p.folders} />
                <Row k="Concept" v={p.concept} />
                <div className="flex gap-3">
                  <dt className="w-16 shrink-0 text-muted-fg">Repos</dt>
                  <dd className="flex flex-wrap gap-1">
                    {p.repos.length ? (
                      p.repos.map((r) => (
                        <Badge key={r} variant="outline">
                          <GitBranch /> {r}
                        </Badge>
                      ))
                    ) : (
                      <span className="text-muted-fg">vault notes only</span>
                    )}
                  </dd>
                </div>
              </dl>
            </Card>
          ))}
        </div>
      )}
      <AddProjectDialog
        open={adding}
        onOpenChange={setAdding}
        onAdjust={(spec) => {
          setAdding(false);
          setAdjust(spec);
          setEditing('new');
        }}
        onSaved={() => void qc.invalidateQueries({ queryKey: ['projects'] })}
      />
      <ProjectDialog
        project={editing}
        initial={adjust}
        onClose={() => {
          setEditing(null);
          setAdjust(undefined);
        }}
        onSaved={() => void qc.invalidateQueries({ queryKey: ['projects'] })}
      />
    </div>
  );
}

function Row({ k, v }: { k: string; v: string[] }) {
  return (
    <div className="flex gap-3">
      <dt className="w-16 shrink-0 text-muted-fg">{k}</dt>
      <dd className="min-w-0 space-y-0.5">
        {v.length ? v.map((x) => <div key={x} className="truncate font-mono text-[11px]">{x}</div>) : <span className="text-muted-fg">—</span>}
      </dd>
    </div>
  );
}

export const EMPTY = { id: '', name: '', notes: [] as string[], folders: [] as string[], repos: [] as string[], concept: [] as string[] };

function ProjectDialog({
  project, initial, onClose, onSaved,
}: {
  project: Project | 'new' | null;
  initial?: typeof EMPTY;
  onClose: () => void;
  onSaved: () => void;
}) {
  const isNew = project === 'new';
  const [f, setF] = useState(EMPTY);
  const [check, setCheck] = useState<ProjectCheck | null>(null);

  useEffect(() => {
    if (project === 'new') setF(initial ?? EMPTY);
    else if (project) setF({ id: project.id, name: project.name, notes: project.notes, folders: project.folders, repos: project.repos, concept: project.concept });
    setCheck(null);
  }, [project, initial]);

  // A live check of every path, a moment after typing stops.
  useEffect(() => {
    if (!project) return;
    const t = setTimeout(() => {
      const spec = isNew ? f : { editing: f.id, name: f.name, notes: f.notes, folders: f.folders, repos: f.repos, concept: f.concept };
      api.checkProject(spec).then(setCheck).catch(() => setCheck(null));
    }, 400);
    return () => clearTimeout(t);
  }, [f, project, isNew]);

  const save = useMutation({
    mutationFn: () =>
      isNew
        ? api.addProject(f)
        : api.editProject(f.id, { name: f.name, notes: f.notes, folders: f.folders, repos: f.repos, concept: f.concept }),
    onSuccess: (r) => {
      saved(r, isNew ? `${f.name} added` : `${f.name} saved`);
      onSaved();
      onClose();
    },
    onError: (e) => toast.error('Not saved', { description: e instanceof ApiError ? e.errors.join('; ') || e.message : String(e) }),
  });
  const errors = (check?.errors ?? []).filter((e) => isNew || !e.includes('already exists'));
  const ready = !!f.name && f.notes.length > 0 && f.folders.length > 0 && (!isNew || !!f.id) && errors.length === 0;

  return (
    <Dialog open={!!project} onOpenChange={(v) => !v && onClose()}>
      <DialogContent className="max-w-2xl">
        <DialogHeader
          title={isNew ? 'Add a project' : `Edit ${f.name}`}
          description="Paths are inside the vault; a * matches a note name. Repos are your own github.com/nonvis repos under ~/nonvis or ~/projects."
        />
        <div className="grid gap-3 sm:grid-cols-2">
          <div className="space-y-1.5">
            <Label htmlFor="pn">Name</Label>
            <Input
              id="pn"
              value={f.name}
              placeholder="Envoy"
              onChange={(e) =>
                setF({ ...f, name: e.target.value, id: isNew ? e.target.value.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '') : f.id })
              }
            />
          </div>
          <div className="space-y-1.5">
            <Label htmlFor="pi">Id</Label>
            <Input id="pi" value={f.id} disabled={!isNew} className="font-mono" onChange={(e) => setF({ ...f, id: e.target.value })} />
          </div>
        </div>
        <ListField label="Briefing note(s)" placeholder="01_Projects/Envoy/05 - *.md" value={f.notes} onChange={(notes) => setF({ ...f, notes })}
          status={(check?.notes ?? []).map((n) => ({ key: n.pattern, ok: n.matches.length > 0, hint: n.matches[0] ?? 'no note matches' }))} />
        <ListField label="Vault folder(s)" placeholder="01_Projects/Envoy" value={f.folders} onChange={(folders) => setF({ ...f, folders })}
          status={(check?.folders ?? []).map((n) => ({ key: n.path, ok: n.exists, hint: n.exists ? 'folder' : 'not a folder' }))} />
        <ListField label="Repos" placeholder="envoy" value={f.repos} onChange={(repos) => setF({ ...f, repos })}
          status={(check?.repos ?? []).map((r) => ({ key: r.name, ok: r.own, hint: r.path ? (r.own ? r.remote : `not yours (#99): ${r.remote || 'no origin'}`) : 'not found' }))} />
        <ListField label="Concept note (optional)" placeholder="01_Projects/Envoy/00 - *.md" value={f.concept} onChange={(concept) => setF({ ...f, concept })}
          status={(check?.concept ?? []).map((n) => ({ key: n.pattern, ok: n.matches.length > 0, hint: n.matches[0] ?? 'no note matches' }))} />
        {errors.length > 0 && (
          <div className="space-y-1 rounded-lg bg-danger/10 p-3 text-xs text-danger">
            {errors.map((e) => (
              <div key={e} className="flex gap-1.5">
                <AlertTriangle className="mt-px size-3.5 shrink-0" /> {e}
              </div>
            ))}
          </div>
        )}
        <div className="flex justify-end gap-2 pt-1">
          <Button variant="outline" onClick={onClose}>
            Cancel
          </Button>
          <Button disabled={!ready || save.isPending} onClick={() => save.mutate()}>
            {isNew ? 'Add and commit' : 'Save and commit'}
          </Button>
        </div>
      </DialogContent>
    </Dialog>
  );
}

function ListField({
  label, placeholder, value, onChange, status,
}: {
  label: string;
  placeholder: string;
  value: string[];
  onChange: (v: string[]) => void;
  status: { key: string; ok: boolean; hint: string }[];
}) {
  const [draft, setDraft] = useState('');
  const add = () => {
    const v = draft.trim();
    if (v && !value.includes(v)) onChange([...value, v]);
    setDraft('');
  };
  return (
    <div className="space-y-1.5">
      <Label>{label}</Label>
      {value.length > 0 && (
        <div className="space-y-1">
          {value.map((v) => {
            const s = status.find((x) => x.key === v);
            return (
              <div key={v} className="flex items-center gap-2 rounded-lg border bg-muted/40 px-2.5 py-1.5">
                {s ? (
                  s.ok ? <CheckCircle2 className="size-3.5 shrink-0 text-ok" /> : <XCircle className="size-3.5 shrink-0 text-danger" />
                ) : (
                  <span className="size-3.5 shrink-0" />
                )}
                <span className="min-w-0 flex-1">
                  <span className="block truncate font-mono text-xs">{v}</span>
                  {s && <span className={cn('block truncate text-[10px]', s.ok ? 'text-muted-fg' : 'text-danger')}>{s.hint}</span>}
                </span>
                <button type="button" onClick={() => onChange(value.filter((x) => x !== v))} className="text-muted-fg hover:text-fg" aria-label={`Remove ${v}`}>
                  <X className="size-3.5" />
                </button>
              </div>
            );
          })}
        </div>
      )}
      <Input
        value={draft}
        placeholder={placeholder}
        className="font-mono text-xs"
        onChange={(e) => setDraft(e.target.value)}
        onKeyDown={(e) => {
          if (e.key === 'Enter') {
            e.preventDefault();
            add();
          }
        }}
        onBlur={add}
      />
    </div>
  );
}
