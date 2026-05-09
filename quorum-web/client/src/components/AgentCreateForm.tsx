import { useState } from "react";
import { createAgent } from "../api";

const ROLES = ["leader", "thinker", "doer", "reviewer", "scribe", "librarian", "evaluator"] as const;

const ROLE_COLORS: Record<string, { active: string; inactive: string }> = {
  leader:    { active: "bg-purple-600 text-white", inactive: "bg-zinc-800 text-purple-400 hover:bg-zinc-700" },
  thinker:   { active: "bg-blue-600 text-white",   inactive: "bg-zinc-800 text-blue-400 hover:bg-zinc-700" },
  doer:      { active: "bg-green-600 text-white",  inactive: "bg-zinc-800 text-green-400 hover:bg-zinc-700" },
  reviewer:  { active: "bg-yellow-600 text-white", inactive: "bg-zinc-800 text-yellow-400 hover:bg-zinc-700" },
  scribe:    { active: "bg-cyan-600 text-white",   inactive: "bg-zinc-800 text-cyan-400 hover:bg-zinc-700" },
  librarian: { active: "bg-pink-600 text-white",   inactive: "bg-zinc-800 text-pink-400 hover:bg-zinc-700" },
  evaluator: { active: "bg-indigo-600 text-white", inactive: "bg-zinc-800 text-indigo-400 hover:bg-zinc-700" },
};

export function AgentCreateForm({ onCreated }: { onCreated: () => void }) {
  const [open, setOpen] = useState(false);
  const [role, setRole] = useState<string>("doer");
  const [name, setName] = useState("");
  const [description, setDescription] = useState("");
  const [targetDir, setTargetDir] = useState("");
  const [skill, setSkill] = useState("");
  const [loading, setLoading] = useState(false);

  const resetForm = () => {
    setRole("doer");
    setName("");
    setDescription("");
    setTargetDir("");
    setSkill("");
  };

  const handleCreate = async () => {
    if (!name.trim()) return;
    setLoading(true);
    try {
      const params: Parameters<typeof createAgent>[0] = {
        role,
        name: name.trim(),
      };
      if (description.trim()) params.description = description.trim();
      if (role === "doer" && targetDir.trim()) params.targetDir = targetDir.trim();
      if (skill.trim()) params.skill = skill.trim();
      const res = await createAgent(params);
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
        + Add Agent
      </button>
    );
  }

  return (
    <div className="bg-zinc-900 border border-zinc-800 rounded-lg p-4 space-y-3">
      <div>
        <label className="text-xs text-zinc-500 block mb-1">Role</label>
        <div className="flex items-center gap-1 flex-wrap">
          {ROLES.map((r) => {
            const colors = ROLE_COLORS[r];
            const isActive = role === r;
            return (
              <button
                key={r}
                onClick={() => setRole(r)}
                className={`px-3 py-1 text-xs rounded-full transition-colors ${
                  isActive ? colors.active : colors.inactive
                }`}
              >
                {r}
              </button>
            );
          })}
        </div>
      </div>

      <div>
        <label className="text-xs text-zinc-500 block mb-1">Name</label>
        <input
          type="text"
          value={name}
          onChange={(e) => setName(e.target.value)}
          placeholder="e.g. my-analyst"
          className="w-full px-3 py-1.5 bg-zinc-800 border border-zinc-700 rounded text-sm text-white placeholder-zinc-500 focus:outline-none focus:border-zinc-500"
        />
      </div>

      <div>
        <label className="text-xs text-zinc-500 block mb-1">Description (optional)</label>
        <input
          type="text"
          value={description}
          onChange={(e) => setDescription(e.target.value)}
          placeholder="What does this agent do?"
          className="w-full px-3 py-1.5 bg-zinc-800 border border-zinc-700 rounded text-sm text-white placeholder-zinc-500 focus:outline-none focus:border-zinc-500"
        />
      </div>

      {role === "doer" && (
        <div>
          <label className="text-xs text-zinc-500 block mb-1">Target directory (optional)</label>
          <input
            type="text"
            value={targetDir}
            onChange={(e) => setTargetDir(e.target.value)}
            placeholder="~/path/to/working/dir"
            className="w-full px-3 py-1.5 bg-zinc-800 border border-zinc-700 rounded text-sm text-white placeholder-zinc-500 focus:outline-none focus:border-zinc-500"
          />
        </div>
      )}

      {role === "doer" && (
        <div>
          <label className="text-xs text-zinc-500 block mb-1">Skill file (optional)</label>
          <input
            type="text"
            value={skill}
            onChange={(e) => setSkill(e.target.value)}
            placeholder="e.g. sui-move or path/to/SKILL.md"
            className="w-full px-3 py-1.5 bg-zinc-800 border border-zinc-700 rounded text-sm text-white placeholder-zinc-500 focus:outline-none focus:border-zinc-500"
          />
        </div>
      )}

      <div className="flex items-center gap-2 pt-1">
        <button
          onClick={handleCreate}
          disabled={loading || !name.trim()}
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
