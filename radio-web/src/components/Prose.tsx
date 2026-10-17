import { blocks, inline, type Inline } from '@/lib/prose';
import { cn } from '@/lib/utils';

export interface Cite {
  path: string;
  line: number;
  end: number | null;
}

/** The tutor's answer: markdown-lite, every resolvable `path:line` a button that opens the code (QR-42). */
export function Prose({
  text, valid, onCite, className,
}: { text: string; valid?: Map<string, string>; onCite?: (c: Cite) => void; className?: string }) {
  const run = (s: string) => <Inlines parts={inline(s)} valid={valid} onCite={onCite} />;
  return (
    <div className={cn('space-y-3 text-sm leading-relaxed', className)}>
      {blocks(text).map((b, i) =>
        b.t === 'code' ? (
          <pre key={i} className="overflow-x-auto rounded-lg bg-muted px-3 py-2 font-mono text-xs leading-relaxed">
            {b.text}
          </pre>
        ) : b.t === 'h' ? (
          <h4 key={i} className="font-semibold">{run(b.text)}</h4>
        ) : b.t === 'ul' ? (
          <ul key={i} className="list-disc space-y-1 pl-5">{b.items.map((x, k) => <li key={k}>{run(x)}</li>)}</ul>
        ) : b.t === 'ol' ? (
          <ol key={i} className="list-decimal space-y-1 pl-5">{b.items.map((x, k) => <li key={k}>{run(x)}</li>)}</ol>
        ) : (
          <p key={i}>{run(b.text)}</p>
        ),
      )}
    </div>
  );
}

function Inlines({ parts, valid, onCite }: { parts: Inline[]; valid?: Map<string, string>; onCite?: (c: Cite) => void }) {
  return (
    <>
      {parts.map((p, i) => {
        if (p.t === 'bold') return <strong key={i}>{p.text}</strong>;
        if (p.t === 'code') return <code key={i} className="rounded bg-muted px-1 py-0.5 font-mono text-[0.85em]">{p.text}</code>;
        if (p.t === 'cite') {
          const resolved = valid?.get(`${p.path}:${p.line}`);
          if (resolved && onCite)
            return (
              <button
                key={i}
                type="button"
                onClick={() => onCite({ path: resolved, line: p.line, end: p.end })}
                className="rounded bg-accent-soft px-1 py-0.5 font-mono text-[0.85em] text-accent underline-offset-2 hover:underline"
              >
                {p.text}
              </button>
            );
          return p.code ? (
            <code key={i} className="rounded bg-muted px-1 py-0.5 font-mono text-[0.85em]">{p.text}</code>
          ) : (
            <span key={i}>{p.text}</span>
          );
        }
        return <span key={i}>{p.text}</span>;
      })}
    </>
  );
}
