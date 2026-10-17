import { useMemo, useState } from 'react';
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { toast } from 'sonner';
import { Lock } from 'lucide-react';
import { api, ApiError } from '@/lib/api';
import { cn } from '@/lib/utils';
import { Button } from '@/components/ui/button';
import { Dialog, DialogContent, DialogHeader } from '@/components/ui/dialog';
import { Input, Label } from '@/components/ui/input';

const ACCENT: Record<string, string> = { af: 'US · F', am: 'US · M', bf: 'UK · F', bm: 'UK · M' };

export function AddSpecialtyDialog({
  open,
  onOpenChange,
  onAdded,
}: {
  open: boolean;
  onOpenChange: (v: boolean) => void;
  onAdded: (id: string) => void;
}) {
  const qc = useQueryClient();
  const cast = useQuery({ queryKey: ['cast'], queryFn: api.cast, enabled: open });
  const [name, setName] = useState('');
  const [angle, setAngle] = useState('');
  const [voice, setVoice] = useState('');
  const [pan, setPan] = useState('centre');
  const id = useMemo(() => name.trim().toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 24), [name]);
  const taken = (cast.data?.cast ?? []).some((s) => s.id === id);
  const add = useMutation({
    mutationFn: () => api.addSpecialty({ id, name: name.trim(), voice, pan, angle: angle.trim() }),
    onSuccess: () => {
      toast.success(`${name.trim()} joins the cast`, { description: `Voice ${voice}, bound for life.` });
      void qc.invalidateQueries({ queryKey: ['cast'] });
      onAdded(id);
      onOpenChange(false);
      setName('');
      setAngle('');
      setVoice('');
    },
    onError: (e) => toast.error('Not added', { description: e instanceof ApiError ? e.message : String(e) }),
  });
  const ok = id.length >= 2 && !taken && angle.trim().endsWith('?') && !!voice && !add.isPending;
  const groups = Object.entries(ACCENT).map(([k, label]) => [label, (cast.data?.free_voices ?? []).filter((v) => v.startsWith(k + '_'))] as const);

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="max-w-xl">
        <DialogHeader
          title="Bring a new specialty"
          description="It joins brainstorms only, answers one question from its own angle, and reads the question’s search hits."
        />
        <div className="space-y-1.5">
          <Label htmlFor="sn">Name</Label>
          <Input id="sn" placeholder="Contrarian" value={name} maxLength={30} onChange={(e) => setName(e.target.value)} />
          {id && <p className="font-mono text-[11px] text-muted-fg">id: {id}{taken && <span className="text-danger"> (taken)</span>}</p>}
        </div>
        <div className="space-y-1.5">
          <Label htmlFor="sa">Its angle: the one question it answers</Label>
          <Input id="sa" placeholder="What is the strongest case against it?" value={angle} maxLength={160} onChange={(e) => setAngle(e.target.value)} />
          {angle && !angle.trim().endsWith('?') && <p className="text-[11px] text-danger">End it with a question mark.</p>}
        </div>
        <div className="space-y-1.5">
          <Label>Voice</Label>
          <div className="space-y-2">
            {groups.map(([label, vs]) =>
              vs.length ? (
                <div key={label} className="flex flex-wrap items-center gap-1.5">
                  <span className="w-14 text-[11px] text-muted-fg">{label}</span>
                  {vs.map((v) => (
                    <button
                      key={v}
                      type="button"
                      onClick={() => setVoice(v)}
                      className={cn(
                        'rounded-full border px-2.5 py-0.5 font-mono text-xs',
                        voice === v ? 'border-accent bg-accent-soft text-accent' : 'text-muted-fg hover:text-fg',
                      )}
                    >
                      {v.slice(3)}
                    </button>
                  ))}
                </div>
              ) : null,
            )}
          </div>
          <p className="flex items-center gap-1.5 text-[11px] text-muted-fg">
            <Lock className="size-3" /> A voice is bound for life: never reassigned, even if the specialty retires.
          </p>
        </div>
        <div className="space-y-1.5">
          <Label>Stereo position</Label>
          <div className="flex flex-wrap gap-1.5">
            {(cast.data?.pans ?? []).map((p) => (
              <button
                key={p}
                type="button"
                onClick={() => setPan(p)}
                className={cn(
                  'rounded-full border px-2.5 py-0.5 text-xs',
                  pan === p ? 'border-accent bg-accent-soft text-accent' : 'text-muted-fg hover:text-fg',
                )}
              >
                {p}
              </button>
            ))}
          </div>
        </div>
        <div className="flex justify-end gap-2 pt-2">
          <Button variant="outline" onClick={() => onOpenChange(false)}>
            Cancel
          </Button>
          <Button disabled={!ok} onClick={() => add.mutate()}>
            Add {name.trim() || 'specialty'}
          </Button>
        </div>
      </DialogContent>
    </Dialog>
  );
}
