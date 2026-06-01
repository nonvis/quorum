import { useState } from "react";
import { createAgent, createTeam } from "../api";

// Fresh-init bootstrap: when a project has only the auto-created `leader` and no
// conversations, offer one-click starter presets.
//
//   knowers  — read-only "knower" thinkers (cartographer/architect/historian/recap).
//              Agents only; Tier-2 init is a CLI step (autopilot must be interactive).
//   build    — everyday do-work team: thinker + doer + scribe (+ a build.yaml team).
//   full     — high-stakes/reviewed: thinker + doer + reviewer + scribe (+ full.yaml).
//
// Each agent is created via the existing POST /api/agents path (--no-ai for plain
// roles; the specialty path also runs a zero-token Tier-1 scan). Build/Full also
// write a runnable team YAML so `converse --team <id>` works immediately.

type Status = "pending" | "creating" | "ok" | "fail";

const teamSlug = (name: string) => name.toLowerCase().replace(/\s+/g, "-");

type PresetAgent = {
  name: string;
  role?: string;       // plain role (thinker/doer/reviewer/scribe)
  specialty?: string;  // knower specialty (resolves to a thinker + SKILL server-side)
  targetDir?: boolean; // doer: pass the absolute project path as --target-dir
  blurb: string;
};

type Preset = {
  id: string;
  label: string;
  button: string;      // tailwind classes for the action button
  tagline: string;
  agents: PresetAgent[];
  team?: { name: string; path: string[] }; // optional team YAML to write after
  done: "knowers" | "team";                // which post-create message to show
};

const PRESETS: Preset[] = [
  {
    id: "knowers",
    label: "Knowers",
    button: "bg-amber-600 hover:bg-amber-500",
    tagline: "read-only — where / how / why / what-changed",
    agents: [
      { name: "cartographer", specialty: "cartographer", blurb: "project layout — where is X?" },
      { name: "architect",    specialty: "architect",    blurb: "how components interconnect, with file:line evidence" },
      { name: "historian",    specialty: "historian",    blurb: "decisions & pivots — what/why, with PR/commit provenance" },
      { name: "recap",        specialty: "recap",        blurb: "what changed recently & where you left off" },
    ],
    done: "knowers",
  },
  {
    id: "build",
    label: "Build",
    button: "bg-green-600 hover:bg-green-500",
    tagline: "the everyday do-work team",
    agents: [
      { name: "thinker", role: "thinker", blurb: "plans the approach" },
      { name: "doer",    role: "doer", targetDir: true, blurb: "implements — writes to the project root" },
      { name: "scribe",  role: "scribe", blurb: "records learnings" },
    ],
    team: { name: "Build", path: ["leader", "thinker", "doer", "scribe"] },
    done: "team",
  },
  {
    id: "full",
    label: "Full build",
    button: "bg-indigo-600 hover:bg-indigo-500",
    tagline: "high-stakes / reviewed build",
    agents: [
      { name: "thinker",  role: "thinker", blurb: "plans the approach" },
      { name: "doer",     role: "doer", targetDir: true, blurb: "implements — writes to the project root" },
      { name: "reviewer", role: "reviewer", blurb: "checks the doer's output actually works" },
      { name: "scribe",   role: "scribe", blurb: "records learnings" },
    ],
    team: { name: "Full build", path: ["leader", "thinker", "doer", "reviewer", "scribe"] },
    done: "team",
  },
];

function CopyBlock({ text }: { text: string }) {
  const [copied, setCopied] = useState(false);
  const copy = async () => {
    try {
      await navigator.clipboard.writeText(text);
      setCopied(true);
      setTimeout(() => setCopied(false), 1500);
    } catch {
      /* clipboard unavailable — the text is selectable anyway */
    }
  };
  return (
    <div className="relative group">
      <pre className="bg-zinc-950 border border-zinc-800 rounded p-3 text-[11px] text-zinc-300 font-mono whitespace-pre overflow-x-auto">
        {text}
      </pre>
      <button
        onClick={copy}
        className="absolute top-2 right-2 px-2 py-0.5 text-[10px] rounded bg-zinc-800 text-zinc-400 hover:bg-zinc-700 hover:text-zinc-200 opacity-0 group-hover:opacity-100 transition-opacity"
      >
        {copied ? "copied" : "copy"}
      </button>
    </div>
  );
}

export function BootstrapPanel({
  isFresh,
  projectPath,
  onCreated,
}: {
  isFresh: boolean;
  projectPath: string;
  onCreated: () => void;
}) {
  const [phase, setPhase] = useState<"idle" | "running" | "done">("idle");
  const [active, setActive] = useState<Preset | null>(null);
  const [status, setStatus] = useState<Record<string, Status>>({});
  const [notes, setNotes] = useState<Record<string, string>>({});
  const [teamNote, setTeamNote] = useState<string | null>(null);
  const [teamId, setTeamId] = useState<string | null>(null);

  // Stay mounted (and visible) through running/done even after the agent refetch
  // flips `isFresh` false; only hide when idle and not fresh.
  if (phase === "idle" && !isFresh) return null;

  const runPreset = async (preset: Preset) => {
    setActive(preset);
    setPhase("running");
    setStatus(Object.fromEntries(preset.agents.map((a) => [a.name, "pending"])));
    setNotes({});
    setTeamNote(null);
    setTeamId(null);

    for (const a of preset.agents) {
      setStatus((s) => ({ ...s, [a.name]: "creating" }));
      try {
        const params: Parameters<typeof createAgent>[0] = { name: a.name };
        if (a.specialty) {
          params.specialty = a.specialty;
        } else {
          params.role = a.role!;
          if (a.targetDir) params.targetDir = projectPath;
        }
        const res = await createAgent(params);
        const exists = !res.success && /exist/i.test(res.error ?? "");
        setStatus((s) => ({ ...s, [a.name]: res.success || exists ? "ok" : "fail" }));
        const note = exists ? "already exists" : res.success ? res.output : res.error;
        if (note) setNotes((n) => ({ ...n, [a.name]: note }));
      } catch (e) {
        setStatus((s) => ({ ...s, [a.name]: "fail" }));
        setNotes((n) => ({ ...n, [a.name]: String(e) }));
      }
    }

    if (preset.team) {
      try {
        const t = await createTeam(preset.team.name, preset.team.path);
        if (t.success) {
          setTeamId(t.id ?? null);
          setTeamNote(`team '${t.id}' written → ${preset.team.path.join(" → ")}`);
        } else {
          setTeamNote(t.error ?? "team create failed");
        }
      } catch (e) {
        setTeamNote(String(e));
      }
    }

    onCreated(); // refresh roster + teams
    setPhase("done");
  };

  const icon = (s: Status | undefined) =>
    s === "ok" ? "✓" : s === "fail" ? "✗" : s === "creating" ? "…" : "·";
  const iconColor = (s: Status | undefined) =>
    s === "ok" ? "text-green-400" : s === "fail" ? "text-red-400" : s === "creating" ? "text-amber-400" : "text-zinc-600";

  const okCount = Object.values(status).filter((s) => s === "ok").length;

  // Knower init via autopilot: generate the flight plan, paste the 4 knower
  // tasks into its "## Flight plan" section, then run the supervisor interactively.
  const supervisorInitCmds = `cd ${projectPath}\nquorum supervisor init`;
  const KNOWER_ARTIFACTS: Record<string, string> = {
    cartographer: ".quorum/vaults/cartographer/knowledge/ref-project-index.md",
    architect: ".quorum/vaults/architect/knowledge/ref-architecture-map.md",
    historian: ".quorum/vaults/historian/knowledge/ref-decisions.md",
    recap: ".quorum/vaults/recap/knowledge/ref-recap.md",
  };
  const flightPlan = [
    "## Flight plan",
    ...PRESETS.find((p) => p.id === "knowers")!.agents.flatMap((a, i) => [
      "",
      `### Task ${i + 1}: Build the ${a.name} artifact`,
      `- agent: ${a.name}`,
      "- slices (parallel):",
      `  - Per your ${a.name} SKILL, read your Tier-1 inputs + the root CLAUDE.md, then WRITE ${KNOWER_ARTIFACTS[a.name]}.`,
      `- done when: ${KNOWER_ARTIFACTS[a.name]} exists`,
    ]),
  ].join("\n");
  const kickoffPrompt =
    "Begin the autopilot run. Read ./SUPERVISOR.md, pass the startup gate, then execute the flight plan task by task — fan out each task's slices as subagents equipped with the roster skill, write each artifact, record outcomes, and checkpoint after each task. Continue until every task is done, then write the morning review and stop.";

  return (
    <div className="px-6 py-1">
      <div className="bg-zinc-900 border border-amber-900/40 rounded-lg p-4 space-y-3">
        <div>
          <h3 className="text-sm font-medium text-amber-300">Bootstrap this project</h3>
          <p className="text-xs text-zinc-500 mt-0.5">
            Freshly initialized — only <span className="text-purple-400">leader</span> exists. Pick a starter team.
          </p>
        </div>

        {/* Idle: show all preset cards */}
        {phase === "idle" &&
          PRESETS.map((preset) => (
            <div
              key={preset.id}
              className="flex items-start justify-between gap-4 bg-zinc-950/40 border border-zinc-800 rounded p-3"
            >
              <div className="min-w-0">
                <div className="text-xs font-medium text-zinc-200">
                  {preset.label} <span className="text-zinc-500 font-normal">— {preset.tagline}</span>
                </div>
                <p className="text-[11px] text-zinc-500 mt-1 leading-snug">
                  Creates {preset.agents.map((a) => a.name).join(", ")}
                  {preset.team ? (
                    <>
                      , plus a runnable <code className="text-zinc-300">{teamSlug(preset.team.name)}</code> team
                    </>
                  ) : (
                    <> · <span className="text-zinc-600">no team (read-only)</span></>
                  )}
                </p>
              </div>
              <button
                onClick={() => runPreset(preset)}
                className={`shrink-0 px-4 py-1.5 text-xs text-white rounded ${preset.button}`}
              >
                {preset.team ? "Create team" : "Add agents"}
              </button>
            </div>
          ))}

        {/* Running / done: show the active preset's progress */}
        {phase !== "idle" && active && (
          <div className="space-y-1">
            <div className="text-[11px] text-zinc-400 font-medium">{active.label}</div>
            {active.agents.map((a) => (
              <div key={a.name} className="flex items-center gap-2 text-[11px]">
                <span className={`w-3 text-center ${iconColor(status[a.name])}`}>{icon(status[a.name])}</span>
                <span className="text-zinc-300 w-24">{a.name}</span>
                <span className="text-zinc-600 truncate">{notes[a.name] ?? a.blurb}</span>
              </div>
            ))}
            {teamNote && <div className="text-[11px] text-zinc-500 pl-5 pt-0.5">{teamNote}</div>}
          </div>
        )}

        {/* Post-create message */}
        {phase === "done" && active?.done === "knowers" && (
          <div className="space-y-2 pt-1 border-t border-zinc-800">
            <p className="text-xs text-zinc-300">
              {okCount === active.agents.length ? "Created all 4 knower agents." : `Created ${okCount}/${active.agents.length} knower agents.`}{" "}
              They have deterministic Tier-1 indexes but no knowledge yet. Initialize them with autopilot
              from a terminal (it must be an interactive session — the dashboard can't launch it):
            </p>
            <div>
              <div className="text-[11px] text-zinc-500 mb-1">1 — generate the flight plan:</div>
              <CopyBlock text={supervisorInitCmds} />
            </div>
            <div>
              <div className="text-[11px] text-zinc-500 mb-1">
                2 — replace the <code className="text-zinc-400">## Flight plan</code> section of SUPERVISOR.md with:
              </div>
              <CopyBlock text={flightPlan} />
            </div>
            <div>
              <div className="text-[11px] text-zinc-500 mb-1">3 — run it interactively:</div>
              <CopyBlock text="claude --agent supervisor" />
            </div>
            <div>
              <div className="text-[11px] text-zinc-500 mb-1">4 — when the session opens, paste this to start the run:</div>
              <CopyBlock text={kickoffPrompt} />
            </div>
            <button onClick={() => setPhase("idle")} className="text-[11px] text-zinc-500 hover:text-zinc-300">
              Dismiss
            </button>
          </div>
        )}

        {phase === "done" && active?.done === "team" && (
          <div className="space-y-2 pt-1 border-t border-zinc-800">
            <p className="text-xs text-zinc-300">
              {active.label} team is ready{teamId ? <> — select <span className="text-zinc-100">'{teamId}'</span> in the team bar above</> : null},
              type a goal in the prompt below, and run. The <span className="text-green-400">doer</span> writes to the project root;
              use the <span className="text-amber-400">brainstorm</span> pill to explore read-only first.
            </p>
            {teamId && (
              <div>
                <div className="text-[11px] text-zinc-500 mb-1">…or from a terminal:</div>
                <CopyBlock text={`cd ${projectPath}\nquorum converse --team ${teamId} "your goal here"`} />
              </div>
            )}
            <button onClick={() => setPhase("idle")} className="text-[11px] text-zinc-500 hover:text-zinc-300">
              Dismiss
            </button>
          </div>
        )}
      </div>
    </div>
  );
}
