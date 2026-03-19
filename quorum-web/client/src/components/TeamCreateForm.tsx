import { useState } from "react";
import type { Agent } from "../types";
import { createTeam } from "../api";

const ROLE_COLORS: Record<string, string> = {
  leader: "bg-purple-500",
  thinker: "bg-blue-500",
  doer: "bg-green-500",
  reviewer: "bg-yellow-500",
  scribe: "bg-cyan-500",
  librarian: "bg-pink-500",
};

const ROLE_INITIALS: Record<string, string> = {
  leader: "L",
  thinker: "T",
  doer: "D",
  reviewer: "R",
  scribe: "S",
  librarian: "Lb",
};

export function TeamCreateForm({
  agents,
  onCreated,
}: {
  agents: Agent[];
  onCreated: () => void;
}) {
  const [open, setOpen] = useState(false);
  const [name, setName] = useState("");
  const [path, setPath] = useState<string[]>([]);
  const [loading, setLoading] = useState(false);

  const resetForm = () => {
    setName("");
    setPath([]);
  };

  const handleCreate = async () => {
    if (!name.trim() || path.length === 0) return;
    setLoading(true);
    try {
      const res = await createTeam(name.trim(), path);
      if (res.success) {
        resetForm();
        setOpen(false);
        onCreated();
      }
    } finally {
      setLoading(false);
    }
  };

  if (!open) {
    return (
      <button
        onClick={() => setOpen(true)}
        className="text-xs text-zinc-500 hover:text-zinc-300 px-2 py-1 border border-dashed border-zinc-700 rounded hover:border-zinc-500"
      >
        + Create Team
      </button>
    );
  }

  const pathSet = new Set(path);
  const available = agents.filter((a) => !pathSet.has(a.id));
  const agentMap = new Map(agents.map((a) => [a.id, a]));

  return (
    <div className="bg-zinc-900 border border-zinc-800 rounded-lg p-4 space-y-3">
      <div>
        <label className="text-xs text-zinc-500 block mb-1">Team name</label>
        <input
          type="text"
          value={name}
          onChange={(e) => setName(e.target.value)}
          placeholder="e.g. Quick Build"
          className="w-full px-3 py-1.5 bg-zinc-800 border border-zinc-700 rounded text-sm text-white placeholder-zinc-500 focus:outline-none focus:border-zinc-500"
        />
      </div>

      <div>
        <label className="text-xs text-zinc-500 block mb-1">Agent path (click to add)</label>
        <div className="flex items-center gap-1.5 flex-wrap">
          {available.map((agent) => {
            const dotColor = ROLE_COLORS[agent.role] ?? "bg-zinc-500";
            const initial = ROLE_INITIALS[agent.role] ?? "?";
            return (
              <button
                key={agent.id}
                onClick={() => setPath([...path, agent.id])}
                className="inline-flex items-center gap-1 px-2 py-0.5 text-xs bg-zinc-800 text-zinc-400 rounded hover:bg-zinc-700 hover:text-zinc-300"
              >
                <span className={`w-2 h-2 rounded-full ${dotColor}`} />
                {agent.name}
                <span className="text-zinc-600">({initial})</span>
              </button>
            );
          })}
          {available.length === 0 && agents.length > 0 && (
            <span className="text-xs text-zinc-600">All agents added</span>
          )}
          {agents.length === 0 && (
            <span className="text-xs text-zinc-600">No agents available — create agents first</span>
          )}
        </div>
      </div>

      {path.length > 0 && (
        <div>
          <label className="text-xs text-zinc-500 block mb-1">Current path</label>
          <div className="flex items-center gap-1 flex-wrap">
            {path.map((id, i) => {
              const agent = agentMap.get(id);
              if (!agent) return null;
              const dotColor = ROLE_COLORS[agent.role] ?? "bg-zinc-500";
              return (
                <span key={id} className="inline-flex items-center gap-1">
                  {i > 0 && <span className="text-zinc-600 text-xs">&rarr;</span>}
                  <span className="inline-flex items-center gap-1 px-2 py-0.5 text-xs bg-zinc-800 text-zinc-300 rounded">
                    <span className={`w-2 h-2 rounded-full ${dotColor}`} />
                    {i + 1}. {agent.name}
                    <button
                      onClick={() => setPath(path.filter((_, j) => j !== i))}
                      className="text-zinc-500 hover:text-red-400 ml-0.5"
                    >
                      &times;
                    </button>
                  </span>
                </span>
              );
            })}
          </div>
        </div>
      )}

      <div className="flex items-center gap-2 pt-1">
        <button
          onClick={handleCreate}
          disabled={loading || !name.trim() || path.length === 0}
          className="px-4 py-1.5 text-xs bg-blue-600 text-white rounded hover:bg-blue-500 disabled:opacity-50 disabled:cursor-not-allowed"
        >
          {loading ? "Creating..." : "Create"}
        </button>
        <button
          onClick={() => {
            resetForm();
            setOpen(false);
          }}
          className="px-4 py-1.5 text-xs bg-zinc-700 text-zinc-300 rounded hover:bg-zinc-600"
        >
          Cancel
        </button>
      </div>
    </div>
  );
}
