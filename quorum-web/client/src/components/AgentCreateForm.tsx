import { useState } from "react";
import { createAgent } from "../api";

const ROLES = ["leader", "thinker", "doer", "scribe", "librarian", "evaluator"] as const;

const ROLE_COLORS: Record<string, { active: string; inactive: string }> = {
  leader:    { active: "bg-purple-600 text-white", inactive: "bg-zinc-800 text-purple-400 hover:bg-zinc-700" },
  thinker:   { active: "bg-blue-600 text-white",   inactive: "bg-zinc-800 text-blue-400 hover:bg-zinc-700" },
  doer:      { active: "bg-green-600 text-white",  inactive: "bg-zinc-800 text-green-400 hover:bg-zinc-700" },
  scribe:    { active: "bg-cyan-600 text-white",   inactive: "bg-zinc-800 text-cyan-400 hover:bg-zinc-700" },
  librarian: { active: "bg-pink-600 text-white",   inactive: "bg-zinc-800 text-pink-400 hover:bg-zinc-700" },
  evaluator: { active: "bg-indigo-600 text-white", inactive: "bg-zinc-800 text-indigo-400 hover:bg-zinc-700" },
};

// Read-only "knower" specialties — thinker + a knower SKILL, run in brainstorm
// mode. The server (POST /api/agents) maps each to thinker + the canonical SKILL
// + description (+ a deterministic Tier-1 scan). Mirrors scripts/setup-knowers.sh.
const SPECIALTIES = [
  { id: "cartographer", blurb: "project layout — where is X?" },
  { id: "architect",    blurb: "how components interconnect, with file:line evidence" },
  { id: "historian",    blurb: "decisions & pivots — what/why, with PR/commit provenance" },
  { id: "recap",        blurb: "what changed recently & where you left off" },
] as const;

const SPECIALTY_DESC: Record<string, string> = Object.fromEntries(
  SPECIALTIES.map((s) => [s.id, s.blurb]),
);

export function AgentCreateForm({ onCreated }: { onCreated: () => void }) {
  const [open, setOpen] = useState(false);
  const [role, setRole] = useState<string>("doer");
  const [specialty, setSpecialty] = useState<string | null>(null);
  const [name, setName] = useState("");
  const [description, setDescription] = useState("");
  const [targetDir, setTargetDir] = useState("");
  const [skill, setSkill] = useState("");
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const resetForm = () => {
    setRole("doer");
    setSpecialty(null);
    setName("");
    setDescription("");
    setTargetDir("");
    setSkill("");
    setError(null);
  };

  const pickRole = (r: string) => {
    setRole(r);
    setSpecialty(null);
  };

  const pickSpecialty = (s: string) => {
    setSpecialty(s);
    if (!name.trim()) setName(s); // convenience: name defaults to the specialty
  };

  const handleCreate = async () => {
    if (!name.trim()) return;
    setLoading(true);
    setError(null);
    try {
      const params: Parameters<typeof createAgent>[0] = { name: name.trim() };
      if (specialty) {
        params.specialty = specialty;
      } else {
        params.role = role;
        if (role === "doer" && targetDir.trim()) params.targetDir = targetDir.trim();
        if (skill.trim()) params.skill = skill.trim();
      }
      if (description.trim()) params.description = description.trim();
      const res = await createAgent(params);
      if (res.success) {
        resetForm();
        setOpen(false);
        onCreated();
      } else {
        setError(res.error || "Failed to create agent");
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
            const isActive = !specialty && role === r;
            return (
              <button
                key={r}
                onClick={() => pickRole(r)}
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
        <label className="text-xs text-zinc-500 block mb-1">
          Specialty <span className="text-zinc-600">(read-only knower — thinker, brainstorm mode)</span>
        </label>
        <div className="flex items-center gap-1 flex-wrap">
          {SPECIALTIES.map((s) => {
            const isActive = specialty === s.id;
            return (
              <button
                key={s.id}
                onClick={() => pickSpecialty(s.id)}
                title={s.blurb}
                className={`px-3 py-1 text-xs rounded-full transition-colors ${
                  isActive
                    ? "bg-amber-600 text-white"
                    : "bg-zinc-800 text-amber-400 hover:bg-zinc-700"
                }`}
              >
                {s.id}
              </button>
            );
          })}
        </div>
        {specialty && (
          <p className="text-[11px] text-zinc-500 mt-1.5 leading-snug">
            {SPECIALTY_DESC[specialty]} · created as a read-only <span className="text-blue-400">thinker</span>.
            Run it in brainstorm mode: <code className="text-zinc-400">scripts/run-knower.sh &lt;project&gt; {specialty}</code>
            {specialty !== "architect" && " · a deterministic Tier-1 scan runs on create"}.
          </p>
        )}
      </div>

      <div>
        <label className="text-xs text-zinc-500 block mb-1">Name</label>
        <input
          type="text"
          value={name}
          onChange={(e) => setName(e.target.value)}
          placeholder={specialty ? specialty : "e.g. my-analyst"}
          className="w-full px-3 py-1.5 bg-zinc-800 border border-zinc-700 rounded text-sm text-white placeholder-zinc-500 focus:outline-none focus:border-zinc-500"
        />
      </div>

      <div>
        <label className="text-xs text-zinc-500 block mb-1">
          Description (optional{specialty ? " — defaults to the canonical knower description" : ""})
        </label>
        <input
          type="text"
          value={description}
          onChange={(e) => setDescription(e.target.value)}
          placeholder={specialty ? "(leave blank to use the canonical knower description)" : "What does this agent do?"}
          className="w-full px-3 py-1.5 bg-zinc-800 border border-zinc-700 rounded text-sm text-white placeholder-zinc-500 focus:outline-none focus:border-zinc-500"
        />
      </div>

      {!specialty && role === "doer" && (
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

      {!specialty && role === "doer" && (
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

      {error && (
        <p className="text-xs text-red-400 break-words">{error}</p>
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
