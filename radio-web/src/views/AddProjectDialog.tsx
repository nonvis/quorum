import { useEffect, useState } from 'react';
import { useMutation, useQuery } from '@tanstack/react-query';
import { toast } from 'sonner';
import { AlertTriangle, ArrowLeft, BookOpen, FileText, Folder, GitBranch, SlidersHorizontal, XCircle } from 'lucide-react';
import { api, ApiError } from '@/lib/api';
import type { Derived } from '@/lib/types';
import { cn } from '@/lib/utils';
import { Button } from '@/components/ui/button';
import { Dialog, DialogContent, DialogHeader } from '@/components/ui/dialog';
import { Input, Label } from '@/components/ui/input';
import { saved, type EMPTY } from './ProjectsView';

/** One choice: a repo (or, vault-only, a folder). The rest follows the vault's conventions;
 *  Adjust opens every field. */
export function AddProjectDialog({
  open, onOpenChange, onAdjust, onSaved,
}: {
  open: boolean;
  onOpenChange: (v: boolean) => void;
  onAdjust: (spec: typeof EMPTY) => void;
  onSaved: () => void;
}) {
  const cands = useQuery({ queryKey: ['project-candidates'], queryFn: api.candidates, enabled: open });
  const [from, setFrom] = useState<{ repo?: string; folder?: string } | null>(null);
  const [d, setD] = useState<Derived | null>(null);
  const [name, setName] = useState('');
  const [vaultOnly, setVaultOnly] = useState(false);

  useEffect(() => {
    if (!open) {
      setFrom(null);
      setD(null);
      setVaultOnly(false);
    }
  }, [open]);

  useEffect(() => {
    if (!from) return;
    setD(null);
    api.derive(from).then((r) => {
      setD(r);
      setName(r.spec.name);
    }).catch((e) => toast.error('Could not read the vault', { description: String(e) }));
  }, [from]);

  const add = useMutation({
    mutationFn: () => api.addProject({ ...d!.spec, name: name.trim() || d!.spec.name }),
    onSuccess: (r) => {
      saved(r, `${name || d!.spec.name} added`);
      onSaved();
      onOpenChange(false);
    },
    onError: (e) => toast.error('Not added', { description: e instanceof ApiError ? e.errors.join('; ') || e.message : String(e) }),
  });

  const errors = d?.check.errors ?? [];
  const brief = d?.check.notes[0]?.matches[0];
  const concept = d?.check.concept[0]?.matches[0];

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="max-w-xl">
        {!from ? (
          <>
            <DialogHeader
              title="Add a project"
              description="Pick its repo. The folder, briefing and concept note come from the vault: the folder whose notes name the repo most."
            />
            {cands.isLoading ? (
              <div className="h-32 animate-pulse rounded-lg bg-muted" />
            ) : !vaultOnly ? (
              <div className="space-y-2">
                {(cands.data?.repos ?? []).length === 0 && (
                  <p className="text-sm text-muted-fg">Every repo of yours under ~/nonvis is already in a project.</p>
                )}
                {(cands.data?.repos ?? []).map((c) => (
                  <button
                    key={c.repo}
                    type="button"
                    onClick={() => setFrom({ repo: c.repo })}
                    className="flex w-full items-center gap-3 rounded-lg border bg-card px-3 py-2.5 text-left hover:border-muted-fg/40"
                  >
                    <GitBranch className="size-4 shrink-0 text-muted-fg" />
                    <span className="font-mono text-sm">{c.repo}</span>
                    <span className="ml-auto truncate text-xs text-muted-fg">
                      {c.folder ? `${c.folder.replace('01_Projects/', '')} · ${c.notes} notes` : 'no notes name it'}
                    </span>
                  </button>
                ))}
                <Button variant="ghost" size="sm" onClick={() => setVaultOnly(true)}>
                  <BookOpen /> A vault-only project (no repo)
                </Button>
              </div>
            ) : (
              <div className="space-y-2">
                <div className="flex flex-wrap gap-1.5">
                  {(cands.data?.folders ?? []).map((f) => (
                    <button
                      key={f}
                      type="button"
                      onClick={() => setFrom({ folder: f })}
                      className="rounded-full border px-3 py-1 text-xs text-muted-fg hover:text-fg"
                    >
                      {f.replace('01_Projects/', '')}
                    </button>
                  ))}
                </div>
                <Button variant="ghost" size="sm" onClick={() => setVaultOnly(false)}>
                  <ArrowLeft /> Back to repos
                </Button>
              </div>
            )}
          </>
        ) : (
          <>
            <DialogHeader title={`Add ${name || from.repo || 'project'}`} description="Derived from the vault. Check it, then add." />
            {!d ? (
              <div className="h-40 animate-pulse rounded-lg bg-muted" />
            ) : (
              <div className="space-y-3">
                <div className="space-y-1.5">
                  <Label htmlFor="an">Name</Label>
                  <Input id="an" value={name} onChange={(e) => setName(e.target.value)} />
                  <p className="font-mono text-[11px] text-muted-fg">id: {d.spec.id}</p>
                </div>
                <dl className="space-y-2 rounded-lg border bg-muted/30 p-3 text-sm">
                  <Item icon={<GitBranch />} k="Repo" v={d.spec.repos.join(', ') || 'vault notes only'} />
                  <Item icon={<Folder />} k="Folder" v={d.spec.folders[0] ?? '—'} bad={!d.spec.folders.length} />
                  <Item icon={<FileText />} k="Briefing" v={brief ?? d.spec.notes[0] ?? '—'} bad={!brief} />
                  <Item icon={<BookOpen />} k="Concept" v={concept ?? 'none'} />
                </dl>
                {Object.keys(d.mentions).length > 1 && (
                  <p className="text-xs text-muted-fg">
                    Notes naming {d.spec.repos[0]}:{' '}
                    {Object.entries(d.mentions).map(([f, n]) => `${f.replace('01_Projects/', '')} ${n}`).join(' · ')}
                  </p>
                )}
                {d.warnings.map((w) => (
                  <p key={w} className="flex gap-1.5 text-xs text-[var(--spk-operator)]">
                    <AlertTriangle className="mt-px size-3.5 shrink-0" /> {w}
                  </p>
                ))}
                {errors.map((e) => (
                  <p key={e} className="flex gap-1.5 text-xs text-danger">
                    <XCircle className="mt-px size-3.5 shrink-0" /> {e}
                  </p>
                ))}
              </div>
            )}
            <div className="flex items-center gap-2 pt-1">
              <Button variant="ghost" size="sm" onClick={() => setFrom(null)}>
                <ArrowLeft /> Back
              </Button>
              <Button variant="ghost" size="sm" disabled={!d} onClick={() => d && onAdjust({ ...d.spec, name })}>
                <SlidersHorizontal /> Adjust
              </Button>
              <Button className="ml-auto" disabled={!d || errors.length > 0 || add.isPending} onClick={() => add.mutate()}>
                Add and commit
              </Button>
            </div>
          </>
        )}
      </DialogContent>
    </Dialog>
  );
}

function Item({ icon, k, v, bad }: { icon: React.ReactNode; k: string; v: string; bad?: boolean }) {
  return (
    <div className="flex items-start gap-2.5">
      <span className="mt-0.5 text-muted-fg [&_svg]:size-4">{icon}</span>
      <dt className="w-16 shrink-0 text-muted-fg">{k}</dt>
      <dd className={cn('min-w-0 break-words font-mono text-xs leading-5', bad && 'text-danger')}>{v}</dd>
    </div>
  );
}
