import { useState, useRef, useEffect } from "react";
import { selectProject, initProject } from "../api";

// Project open/switch/init, re-homed into the top bar as a breadcrumb dropdown.
export function ProjectSwitcher({
  current,
  recent,
  onSelect,
}: {
  current: string | null;
  recent: string[];
  onSelect: (path: string) => void;
}) {
  const [open, setOpen] = useState(false);
  const [input, setInput] = useState("");
  const [initNeeded, setInitNeeded] = useState(false);
  const ref = useRef<HTMLDivElement>(null);

  const basename = (p: string) => p.split("/").filter(Boolean).pop() ?? p;
  const otherRecent = recent.filter((p) => p !== current);

  useEffect(() => {
    if (!open) return;
    const onDoc = (e: MouseEvent) => {
      if (ref.current && !ref.current.contains(e.target as Node)) setOpen(false);
    };
    document.addEventListener("mousedown", onDoc);
    return () => document.removeEventListener("mousedown", onDoc);
  }, [open]);

  const pick = async (path: string) => {
    const res = await selectProject(path);
    if (res.error?.includes("No .quorum/")) {
      setInitNeeded(true);
    } else if (res.success) {
      onSelect(path);
      setInput("");
      setInitNeeded(false);
      setOpen(false);
    }
  };

  return (
    <div ref={ref} className="relative">
      <button
        onClick={() => setOpen((v) => !v)}
        className="inline-flex items-center gap-1.5 rounded-lg border border-line-soft bg-chip px-2.5 py-[3px] font-mono text-xs text-[#b5aebf] hover:border-line-dash"
        title={current ?? "Select a project"}
      >
        <span>📁</span>
        <span className="text-ink">{current ? basename(current) : "select project"}</span>
        <span className="text-faint">▾</span>
      </button>

      {open && (
        <div className="absolute left-0 top-full z-[70] mt-2 w-80 rounded-xl border border-line-edge bg-sheet p-3 shadow-2xl">
          <form
            onSubmit={(e) => {
              e.preventDefault();
              if (input.trim()) pick(input.trim());
            }}
            className="flex gap-2"
          >
            <input
              value={input}
              onChange={(e) => setInput(e.target.value)}
              placeholder="~/path/to/project"
              title="Absolute or ~/-prefixed path"
              className="min-w-0 flex-1 rounded-lg border border-line-soft bg-field px-2.5 py-1.5 text-xs text-ink outline-none focus:border-faint"
            />
            <button
              type="submit"
              disabled={!input.trim()}
              className="rounded-lg bg-brand px-3 py-1.5 text-xs font-bold text-[#1a1410] hover:bg-brand-bright disabled:opacity-45"
            >
              Open
            </button>
          </form>

          {initNeeded && (
            <div className="mt-2 flex items-center gap-2 rounded-lg border border-line-dash px-2.5 py-2">
              <span className="text-xs text-brand">No .quorum/ here.</span>
              <button
                onClick={async () => {
                  const res = await initProject(input.trim());
                  if (res.success) {
                    onSelect(input.trim());
                    setInput("");
                    setInitNeeded(false);
                    setOpen(false);
                  }
                }}
                className="ml-auto rounded-lg bg-running px-2.5 py-1 text-xs font-semibold text-[#0f1220] hover:opacity-90"
              >
                Initialize
              </button>
            </div>
          )}

          {otherRecent.length > 0 && (
            <div className="mt-3">
              <div className="mb-1.5 font-mono text-[10px] tracking-[0.12em] text-faint">RECENT</div>
              <div className="flex flex-col gap-0.5">
                {otherRecent.map((p) => (
                  <button
                    key={p}
                    onClick={() => pick(p)}
                    title={p}
                    className="truncate rounded-md px-2 py-1.5 text-left font-mono text-xs text-muted hover:bg-chip hover:text-ink"
                  >
                    {basename(p)}
                    <span className="ml-2 text-[10px] text-dim">{p.replace(/\/[^/]+$/, "")}</span>
                  </button>
                ))}
              </div>
            </div>
          )}
        </div>
      )}
    </div>
  );
}
