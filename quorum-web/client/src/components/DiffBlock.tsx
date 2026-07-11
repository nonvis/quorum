// Diff-style rendering for write payloads (VAULT_UPDATE bodies, code writes).
// "Less reading": a write is a change — show it as one, not as prose.
//
// Two shapes:
//  - unified diff (has @@ hunks or +/- line pairs) → per-line coloring
//  - full-content write → every line gutter-marked "+" (it all lands verbatim)

const LINE_STYLE: Record<string, { color: string; bg: string }> = {
  add: { color: "#85bd93", bg: "rgba(133,189,147,0.07)" },
  del: { color: "#c98b81", bg: "rgba(201,139,129,0.07)" },
  hunk: { color: "#8fa9e8", bg: "rgba(143,169,232,0.07)" },
  meta: { color: "#8a8390", bg: "transparent" },
  ctx: { color: "#9b94a3", bg: "transparent" },
};

function classify(line: string): keyof typeof LINE_STYLE {
  if (line.startsWith("@@")) return "hunk";
  if (line.startsWith("+++") || line.startsWith("---") || line.startsWith("diff ") || line.startsWith("index "))
    return "meta";
  if (line.startsWith("+")) return "add";
  if (line.startsWith("-")) return "del";
  return "ctx";
}

export function looksLikeUnifiedDiff(text: string): boolean {
  if (/^@@ -\d/m.test(text)) return true;
  // +/- pairs without hunk headers (e.g. hand-written mini-diffs). Require
  // both signs so markdown bullet lists ("- item") don't false-positive.
  return /^\+(?!\+)/m.test(text) && /^-(?!-)/m.test(text);
}

export function DiffBlock({ text, forceWrite }: { text: string; forceWrite?: boolean }) {
  const isDiff = !forceWrite && looksLikeUnifiedDiff(text);
  const lines = text.replace(/\n$/, "").split("\n");

  return (
    <div className="max-h-96 overflow-auto bg-field font-mono text-[11.5px] leading-[1.65]">
      <table className="w-full border-collapse">
        <tbody>
          {lines.map((line, i) => {
            const kind = isDiff ? classify(line) : "add";
            const st = LINE_STYLE[kind];
            const gutter = isDiff
              ? kind === "add"
                ? "+"
                : kind === "del"
                  ? "-"
                  : ""
              : "+";
            const body = isDiff && (kind === "add" || kind === "del") ? line.slice(1) : line;
            return (
              <tr key={i} style={{ background: st.bg }}>
                <td
                  className="w-6 select-none px-1.5 text-right align-top"
                  style={{ color: st.color, opacity: 0.75 }}
                >
                  {gutter}
                </td>
                <td className="whitespace-pre-wrap break-all pr-3 align-top" style={{ color: st.color }}>
                  {body || " "}
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
    </div>
  );
}
