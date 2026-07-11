// Quorum protocol blocks are fenced code blocks (```HANDOFF / ```VAULT_UPDATE)
// which Markdown would render as literal code — hiding the inner content. We
// pre-extract them so the bodies can render with full Markdown support and the
// handoff/vault targets get their own framed boxes.
import type { Task } from "../types";

export type Segment =
  | { kind: "prose"; text: string }
  | { kind: "handoff"; to: string; prompt: string }
  | { kind: "vault"; path: string; content: string };

// Parse a keyed protocol-block body: a `<keyName>:` line and a `<bodyName>: |`
// (or inline) multi-line field. Shared by HANDOFF (to/prompt) and VAULT_UPDATE
// (path/content) — both use the same `key: val` + `body: |` indented shape.
function parseKeyedBlock(
  inner: string,
  keyName: string,
  bodyName: string,
): { key: string; body: string } {
  const lines = inner.split("\n");
  let key = "";
  let body = "";
  let collecting = false;
  let indent = 0;
  const keyRe = new RegExp(`^\\s*${keyName}:\\s*(.+)$`);
  const bodyRe = new RegExp(`^\\s*${bodyName}:\\s*(.*)$`);
  for (let i = 0; i < lines.length; i++) {
    const ln = lines[i];
    if (!collecting) {
      const km = ln.match(keyRe);
      if (km && !key) {
        key = km[1].trim();
        continue;
      }
      const bm = ln.match(bodyRe);
      if (bm) {
        const rest = bm[1].trim();
        if (rest === "|") {
          collecting = true;
          indent = 2;
        } else {
          body = rest;
          collecting = true;
          indent = 0;
        }
      }
    } else {
      let line = ln;
      if (indent > 0 && line.startsWith(" ".repeat(indent))) {
        line = line.slice(indent);
      }
      body += (body.length > 0 ? "\n" : "") + line;
    }
  }
  return { key, body };
}

export function parseSegments(text: string): Segment[] {
  const segments: Segment[] = [];
  const re = /```(HANDOFF|VAULT_UPDATE)\s*\n([\s\S]*?)\n?```/g;
  let last = 0;
  let m: RegExpExecArray | null;
  while ((m = re.exec(text)) !== null) {
    if (m.index > last) {
      const prose = text.slice(last, m.index);
      if (prose.trim().length > 0) segments.push({ kind: "prose", text: prose });
    }
    if (m[1] === "HANDOFF") {
      const { key, body } = parseKeyedBlock(m[2], "to", "prompt");
      segments.push({ kind: "handoff", to: key, prompt: body });
    } else {
      const { key, body } = parseKeyedBlock(m[2], "path", "content");
      segments.push({ kind: "vault", path: key, content: body });
    }
    last = re.lastIndex;
  }
  if (last < text.length) {
    const tail = text.slice(last);
    if (tail.trim().length > 0) segments.push({ kind: "prose", text: tail });
  }
  return segments;
}

// A leader turn created by `quorum respond` carries the operator's reply as a
// `# Human Response` section inside the assembled prompt. Pull it out so the
// UI can show "you responded: <text>" on that task.
export function extractHumanResponse(prompt: string | null | undefined): string | null {
  if (!prompt) return null;
  const marker = "# Human Response";
  const idx = prompt.indexOf(marker);
  if (idx < 0) return null;
  const after = prompt.slice(idx + marker.length).replace(/^\s*\n+/, "");
  const m = after.match(/^([\s\S]*?)(?:\n#{1,6}\s|\n---|\s*$)/);
  return (m ? m[1] : after).trim() || null;
}

// The leader's gate question lives in the last `HANDOFF to: human` block across
// the conversation's task results. Pull its prompt so the respond panel can show
// what's being asked, instead of the (often empty) paused_reason.
export function lastHumanGateMessage(tasks: Task[]): string | null {
  for (let i = tasks.length - 1; i >= 0; i--) {
    const result = tasks[i]?.result;
    if (!result) continue;
    const segs = parseSegments(result);
    for (let j = segs.length - 1; j >= 0; j--) {
      const s = segs[j];
      if (s.kind === "handoff" && s.to === "human") return s.prompt;
    }
  }
  return null;
}
