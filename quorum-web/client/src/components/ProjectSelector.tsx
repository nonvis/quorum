import { useState } from "react";
import { selectProject, initProject } from "../api";

export function ProjectSelector({
  current,
  recent,
  onSelect,
}: {
  current: string | null;
  recent: string[];
  onSelect: (path: string) => void;
}) {
  const [input, setInput] = useState("");
  const [showRecent, setShowRecent] = useState(false);
  const [initNeeded, setInitNeeded] = useState(false);

  const basename = (p: string) => p.split("/").filter(Boolean).pop() ?? p;

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!input.trim()) return;
    const result = await selectProject(input.trim());
    if (result.error?.includes("No .quorum/")) {
      setInitNeeded(true);
    } else if (result.success) {
      onSelect(input.trim());
      setInput("");
      setInitNeeded(false);
    }
  };

  // Other recent projects (exclude current)
  const otherRecent = recent.filter((p) => p !== current);

  return (
    <div className="bg-zinc-900/50 border-b border-zinc-800 px-6 py-2">
      <div className="flex items-center justify-between">
        <div className="flex items-center gap-2">
          <span className="text-sm">&#128194;</span>
          {current ? (
            <div className="flex items-center gap-1">
              <span
                className="text-sm font-mono text-white"
                title={current}
              >
                {basename(current)}
              </span>
              {otherRecent.length > 0 && (
                <button
                  onClick={() => setShowRecent(!showRecent)}
                  className="text-xs text-zinc-500 hover:text-zinc-300 ml-1"
                >
                  &#9662;
                </button>
              )}
            </div>
          ) : (
            <span className="text-sm text-zinc-500">No project selected</span>
          )}
        </div>
        <form onSubmit={handleSubmit} className="flex items-center gap-2">
          <input
            type="text"
            value={input}
            onChange={(e) => setInput(e.target.value)}
            placeholder="~/path/to/project (~ expanded to $HOME)"
            title="Absolute or ~/-prefixed path. ~ is expanded to your home directory."
            className="w-64 px-3 py-1 bg-zinc-800 border border-zinc-700 rounded text-sm text-white placeholder-zinc-500 focus:outline-none focus:border-zinc-500"
          />
          <button
            type="submit"
            disabled={!input.trim()}
            className="px-3 py-1 text-xs bg-zinc-700 text-white rounded hover:bg-zinc-600 disabled:opacity-50 disabled:cursor-not-allowed"
          >
            Open
          </button>
        </form>
      </div>

      {/* Recent projects: show when no current project, or when dropdown toggled */}
      {((!current && recent.length > 0) || (current && showRecent && otherRecent.length > 0)) && (
        <div className="flex items-center gap-2 mt-2 flex-wrap">
          <span className="text-xs text-zinc-500">Recent:</span>
          {(current ? otherRecent : recent).map((path) => (
            <button
              key={path}
              onClick={async () => {
                await selectProject(path);
                onSelect(path);
                setShowRecent(false);
              }}
              title={path}
              className="px-2 py-0.5 text-xs bg-zinc-800 text-zinc-400 rounded hover:bg-zinc-700 hover:text-zinc-300"
            >
              {basename(path)}
            </button>
          ))}
        </div>
      )}

      {initNeeded && (
        <div className="flex items-center gap-2 mt-2">
          <span className="text-sm text-amber-400">
            No .quorum/ found at this path.
          </span>
          <button
            onClick={async () => {
              const res = await initProject(input.trim());
              if (res.success) {
                onSelect(input.trim());
                setInput("");
                setInitNeeded(false);
              }
            }}
            className="px-3 py-1 text-xs bg-blue-600 text-white rounded hover:bg-blue-500"
          >
            Initialize Quorum
          </button>
          <button
            onClick={() => setInitNeeded(false)}
            className="px-3 py-1 text-xs bg-zinc-700 text-zinc-300 rounded hover:bg-zinc-600"
          >
            Cancel
          </button>
        </div>
      )}
    </div>
  );
}
