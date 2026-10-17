/** The tutor's markdown, read as a few block and inline kinds (no HTML is ever injected):
 *  fences, headings, lists, paragraphs; inline code, bold and `path:line` citations (QR-42). */

export type Block =
  | { t: 'p'; text: string }
  | { t: 'h'; text: string }
  | { t: 'code'; lang: string; text: string }
  | { t: 'ul'; items: string[] }
  | { t: 'ol'; items: string[] };

export type Inline =
  | { t: 'text'; text: string }
  | { t: 'code'; text: string }
  | { t: 'bold'; text: string }
  | { t: 'cite'; text: string; path: string; line: number; end: number | null; code: boolean };

export function blocks(src: string): Block[] {
  const out: Block[] = [];
  const lines = src.replace(/\r\n/g, '\n').split('\n');
  let para: string[] = [];
  const flush = () => {
    if (para.length) out.push({ t: 'p', text: para.join(' ') });
    para = [];
  };
  for (let i = 0; i < lines.length; i++) {
    const l = lines[i];
    const fence = l.match(/^\s*```\s*([\w+-]*)\s*$/);
    if (fence) {
      flush();
      const body: string[] = [];
      for (i++; i < lines.length && !/^\s*```\s*$/.test(lines[i]); i++) body.push(lines[i]);
      out.push({ t: 'code', lang: fence[1], text: body.join('\n') });
      continue;
    }
    const h = l.match(/^#{1,6}\s+(.*)$/);
    if (h) {
      flush();
      out.push({ t: 'h', text: h[1].trim() });
      continue;
    }
    const ul = l.match(/^\s*[-*]\s+(.*)$/);
    const ol = l.match(/^\s*\d+[.)]\s+(.*)$/);
    if (ul || ol) {
      flush();
      const t = ul ? 'ul' : 'ol';
      const last = out[out.length - 1];
      const item = (ul ?? ol)![1];
      if (last && last.t === t) last.items.push(item);
      else out.push({ t, items: [item] });
      continue;
    }
    if (!l.trim()) {
      flush();
      continue;
    }
    const last = out[out.length - 1];
    if (!para.length && last && (last.t === 'ul' || last.t === 'ol') && /^\s{2,}/.test(l)) {
      last.items[last.items.length - 1] += ' ' + l.trim();   // a wrapped list item
      continue;
    }
    para.push(l.trim());
  }
  flush();
  return out;
}

const CITE = /((?:[\w.-]+\/)*[\w-][\w.-]*\.[A-Za-z]\w{0,7}):(\d+)(?:-(\d+))?/g;

function cites(text: string, code: boolean): Inline[] {
  const out: Inline[] = [];
  let at = 0;
  for (const m of text.matchAll(CITE)) {
    const i = m.index ?? 0;
    if (i > 0 && /[\w/.-]/.test(text[i - 1])) continue;
    if (i > at) out.push(code ? { t: 'code', text: text.slice(at, i) } : { t: 'text', text: text.slice(at, i) });
    out.push({ t: 'cite', text: m[0], path: m[1], line: Number(m[2]), end: m[3] ? Number(m[3]) : null, code });
    at = i + m[0].length;
  }
  if (at < text.length) out.push(code ? { t: 'code', text: text.slice(at) } : { t: 'text', text: text.slice(at) });
  return out;
}

export function inline(text: string): Inline[] {
  const out: Inline[] = [];
  for (const part of text.split(/(`[^`]+`|\*\*[^*]+\*\*)/)) {
    if (!part) continue;
    if (part.startsWith('`') && part.endsWith('`') && part.length > 1) out.push(...cites(part.slice(1, -1), true));
    else if (part.startsWith('**') && part.endsWith('**') && part.length > 3) out.push({ t: 'bold', text: part.slice(2, -2) });
    else out.push(...cites(part, false));
  }
  return out;
}
