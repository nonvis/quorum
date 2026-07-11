import { useState } from "react";
import { createAgent } from "../api";
import { ROLE } from "../lib/theme";

const ROLES = ["leader", "thinker", "doer", "evaluator"] as const;

const BRAND = "#e3a45c"; // read-only knower / specialty pills

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
        className="text-xs text-muted hover:text-ink px-2 py-1 border border-dashed border-line-soft rounded hover:border-line-dash"
      >
        + Add Agent
      </button>
    );
  }

  return (
    <div className="bg-panel border border-line rounded-lg p-4 space-y-3">
      <div>
        <label className="text-xs text-faint block mb-1">Role</label>
        <div className="flex items-center gap-1 flex-wrap">
          {ROLES.map((r) => {
            const c = ROLE[r];
            const isActive = !specialty && role === r;
            return (
              <button
                key={r}
                onClick={() => pickRole(r)}
                className={`px-3 py-1 text-xs rounded-full border transition-colors ${
                  isActive ? "" : "bg-chip text-muted border-transparent hover:text-ink"
                }`}
                style={
                  isActive
                    ? { color: c, background: `${c}1f`, borderColor: `${c}59` }
                    : undefined
                }
              >
                {r}
              </button>
            );
          })}
        </div>
      </div>

      <div>
        <label className="text-xs text-faint block mb-1">
          Specialty <span className="text-dim">(read-only knower — thinker, brainstorm mode)</span>
        </label>
        <div className="flex items-center gap-1 flex-wrap">
          {SPECIALTIES.map((s) => {
            const isActive = specialty === s.id;
            return (
              <button
                key={s.id}
                onClick={() => pickSpecialty(s.id)}
                title={s.blurb}
                className={`px-3 py-1 text-xs rounded-full border transition-colors ${
                  isActive ? "" : "bg-chip text-muted border-transparent hover:text-ink"
                }`}
                style={
                  isActive
                    ? { color: BRAND, background: `${BRAND}1f`, borderColor: `${BRAND}59` }
                    : undefined
                }
              >
                {s.id}
              </button>
            );
          })}
        </div>
        {specialty && (
          <p className="text-[11px] text-faint mt-1.5 leading-snug">
            {SPECIALTY_DESC[specialty]} · created as a read-only <span className="text-running">thinker</span>.
            Run it in brainstorm mode: <code className="bg-chip text-muted font-mono px-1 rounded">scripts/run-knower.sh &lt;project&gt; {specialty}</code>
            {specialty !== "architect" && " · a deterministic Tier-1 scan runs on create"}.
          </p>
        )}
      </div>

      <div>
        <label className="text-xs text-faint block mb-1">Name</label>
        <input
          type="text"
          value={name}
          onChange={(e) => setName(e.target.value)}
          placeholder={specialty ? specialty : "e.g. my-analyst"}
          className="w-full px-3 py-1.5 bg-field border border-line-soft rounded text-sm text-ink placeholder-dim focus:outline-none focus:border-line-dash"
        />
      </div>

      <div>
        <label className="text-xs text-faint block mb-1">
          Description (optional{specialty ? " — defaults to the canonical knower description" : ""})
        </label>
        <input
          type="text"
          value={description}
          onChange={(e) => setDescription(e.target.value)}
          placeholder={specialty ? "(leave blank to use the canonical knower description)" : "What does this agent do?"}
          className="w-full px-3 py-1.5 bg-field border border-line-soft rounded text-sm text-ink placeholder-dim focus:outline-none focus:border-line-dash"
        />
      </div>

      {!specialty && role === "doer" && (
        <div>
          <label className="text-xs text-faint block mb-1">Target directory (optional)</label>
          <input
            type="text"
            value={targetDir}
            onChange={(e) => setTargetDir(e.target.value)}
            placeholder="~/path/to/working/dir"
            className="w-full px-3 py-1.5 bg-field border border-line-soft rounded text-sm text-ink placeholder-dim focus:outline-none focus:border-line-dash"
          />
        </div>
      )}

      {!specialty && role === "doer" && (
        <div>
          <label className="text-xs text-faint block mb-1">Skill file (optional)</label>
          <input
            type="text"
            value={skill}
            onChange={(e) => setSkill(e.target.value)}
            placeholder="e.g. sui-move or path/to/SKILL.md"
            className="w-full px-3 py-1.5 bg-field border border-line-soft rounded text-sm text-ink placeholder-dim focus:outline-none focus:border-line-dash"
          />
        </div>
      )}

      {error && (
        <p className="text-xs text-closed break-words">{error}</p>
      )}

      <div className="flex items-center gap-2 pt-1">
        <button
          onClick={handleCreate}
          disabled={loading || !name.trim()}
          className="px-4 py-1.5 text-xs bg-brand hover:bg-brand-bright text-[#1a1410] rounded font-medium disabled:opacity-50 disabled:cursor-not-allowed"
        >
          {loading ? "Creating..." : "Create"}
        </button>
        <button
          onClick={() => {
            resetForm();
            setOpen(false);
          }}
          className="px-4 py-1.5 text-xs border border-line-dash bg-transparent text-[#c9c3bd] rounded hover:text-ink"
        >
          Cancel
        </button>
      </div>
    </div>
  );
}
