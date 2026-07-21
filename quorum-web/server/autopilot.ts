// Autopilot (second execution engine) — web bookends.
//
// The web PREPARES a flight (generate SUPERVISOR.md — this module's writer
// matches the deterministic C++ generator in quorum-core/src/cli/
// supervisor_init.h) and REVIEWS a flight (read .quorum/autopilot/ into the
// Flight read model). A flight itself RUNS in a terminal as an interactive
// `claude --agent supervisor` session — the web never launches one.
//
// Contract: templates/specs/autopilot-protocol.md (v0.3). Artifacts:
//   <project>/SUPERVISOR.md                    — generated flight plan (config)
//   <project>/.quorum/autopilot/checkpoint.md  — live flight resume/review state
//   <project>/.quorum/autopilot/archive/*.md   — completed past flights (same
//                                                checkpoint schema; a new plan
//                                                write archives a completed
//                                                checkpoint here)
//   <project>/.quorum/autopilot/reviewed.json  — morning-review "cleared" flags
//                                                (server-side so it survives
//                                                refresh + browser switches)
//
// Fixture files carry a `Fixture: true` header line and render badged as
// synthetic — they are demo data for the morning-review UI until real
// overnight flights accumulate, safe to delete.

import { join, resolve, basename } from "path";
import {
  readFileSync,
  writeFileSync,
  readdirSync,
  existsSync,
  mkdirSync,
  renameSync,
} from "fs";

// ── read model ────────────────────────────────────────────────────────

export type FlightTaskStatus = "pending" | "in_flight" | "done";
export type FlightStatus = "ready" | "in_flight" | "needs_you" | "complete";

export interface FlightTask {
  index: number;
  title: string;
  status: FlightTaskStatus;
  // done-but-with-caveat (outcome mentions a skip/failure) → ⚠ in the ledger
  warn: boolean;
}

export interface FlightOutcome {
  heading: string;
  taskIndex: number | null;
  bullets: string[];
}

export interface MorningReview {
  done: string;
  pending: string;
  blockedOn: string;
  notes: string | null;
}

export interface Flight {
  id: string; // "current" for the live checkpoint, else archive file stem
  source: "checkpoint" | "archive" | "bundled-fixture";
  fixture: boolean;
  project: string;
  name: string;
  mode: string | null; // generic | brainstorm | null (pre-mode flights)
  specVersion: string | null;
  createdAt: string | null;
  updatedAt: string | null;
  status: FlightStatus;
  tasks: FlightTask[];
  outcomes: FlightOutcome[];
  morningReview: MorningReview | null;
  reviewed: boolean;
  launchCommand: string;
}

// ── plan payload (composer → SUPERVISOR.md) ───────────────────────────

export interface PlanTask {
  title: string;
  agent: string;
  slices: string[];
  doneWhen: string;
}

export interface PlanPayload {
  goal: string;
  mode: "generic" | "brainstorm";
  tasks: PlanTask[];
  maxMajorTasks?: number | null;
  force?: boolean;
}

// ── checkpoint parsing ────────────────────────────────────────────────

interface ParsedCheckpoint {
  createdAt: string | null;
  updatedAt: string | null;
  specVersion: string | null;
  fixture: boolean;
  mode: string | null;
  goal: string | null;
  tasks: FlightTask[];
  outcomes: FlightOutcome[];
  morningReview: MorningReview | null;
}

const TASK_LINE = /^-\s*\[([ >x])\]\s*(.+)$/;
const TASK_NUM = /^Task\s+(\d+)\s*:?\s*/i;
const WARN_HINT = /\bSKIPPED\b|\bFAILED\b|\bblocked\b|\bcaveat\b|⚠/i;

function markerStatus(m: string): FlightTaskStatus {
  if (m === "x") return "done";
  if (m === ">") return "in_flight";
  return "pending";
}

export function parseCheckpoint(text: string): ParsedCheckpoint {
  const lines = text.split("\n");
  const out: ParsedCheckpoint = {
    createdAt: null,
    updatedAt: null,
    specVersion: null,
    fixture: false,
    mode: null,
    goal: null,
    tasks: [],
    outcomes: [],
    morningReview: null,
  };

  let section = "";
  let outcome: FlightOutcome | null = null;
  const review: Record<string, string> = {};
  let lastReviewKey = "";

  for (const raw of lines) {
    const line = raw.replace(/\s+$/, "");

    const h2 = line.match(/^##\s+(.+)$/);
    if (h2) {
      section = h2[1].toLowerCase();
      outcome = null;
      continue;
    }

    if (section === "") {
      // header block: `Key: value` lines before the first ## section
      const kv = line.match(/^([A-Za-z][A-Za-z ]*?):\s*(.+)$/);
      if (kv) {
        const key = kv[1].toLowerCase();
        const val = kv[2].trim();
        if (key === "created at") out.createdAt = val;
        else if (key === "updated at") out.updatedAt = val;
        else if (key === "flight spec") out.specVersion = val;
        else if (key === "fixture") out.fixture = /^true/i.test(val);
        else if (key === "mode") out.mode = val.split(/\s/)[0];
        else if (key === "goal") out.goal = val;
      }
      continue;
    }

    if (section.startsWith("major tasks")) {
      const tm = line.match(TASK_LINE);
      if (tm) {
        const title = tm[2].trim();
        const num = title.match(TASK_NUM);
        out.tasks.push({
          index: num ? parseInt(num[1], 10) : out.tasks.length + 1,
          title: title.replace(TASK_NUM, "").trim(),
          status: markerStatus(tm[1]),
          warn: false,
        });
      }
      continue;
    }

    if (section.startsWith("condensed outcomes")) {
      const h3 = line.match(/^###\s+(.+)$/);
      if (h3) {
        const heading = h3[1].trim();
        const num = heading.match(/Task\s+(\d+)/i);
        outcome = {
          heading,
          taskIndex: num ? parseInt(num[1], 10) : null,
          bullets: [],
        };
        out.outcomes.push(outcome);
        continue;
      }
      const bullet = line.match(/^-\s+(.+)$/);
      if (bullet && outcome) {
        outcome.bullets.push(bullet[1].trim());
      } else if (outcome && line.trim() && outcome.bullets.length > 0) {
        // continuation of a wrapped bullet
        outcome.bullets[outcome.bullets.length - 1] += " " + line.trim();
      }
      continue;
    }

    if (section.startsWith("morning review")) {
      const kv = line.match(/^-\s*([a-z-]+):\s*(.*)$/i);
      if (kv) {
        lastReviewKey = kv[1].toLowerCase();
        review[lastReviewKey] = kv[2].trim();
      } else if (line.trim() && lastReviewKey) {
        review[lastReviewKey] += " " + line.trim();
      }
      continue;
    }
  }

  if (Object.keys(review).length > 0) {
    out.morningReview = {
      done: review["done"] ?? "",
      pending: review["pending"] ?? "",
      blockedOn: review["blocked-on"] ?? review["blocked"] ?? "none",
      notes: review["notes"] ?? null,
    };
  }

  // ⚠ derivation: a done task whose condensed outcome mentions a skip/failure.
  for (const t of out.tasks) {
    if (t.status !== "done") continue;
    const oc = out.outcomes.find((o) => o.taskIndex === t.index);
    if (oc && oc.bullets.some((b) => WARN_HINT.test(b))) t.warn = true;
  }

  return out;
}

// ── SUPERVISOR.md parsing (live flight name/mode + plan preview) ──────

interface ParsedPlan {
  goal: string | null;
  mode: string | null;
  projectName: string | null;
  taskTitles: string[];
}

export function parseSupervisorMd(text: string): ParsedPlan {
  const out: ParsedPlan = { goal: null, mode: null, projectName: null, taskTitles: [] };

  const fm = text.match(/^---\n([\s\S]*?)\n---/);
  if (fm) {
    out.goal = fm[1].match(/^goal:\s*(.+)$/m)?.[1]?.trim() ?? null;
    out.mode = fm[1].match(/^mode:\s*(.+)$/m)?.[1]?.trim() ?? null;
  }
  out.projectName = text.match(/^- name:\s*(.+)$/m)?.[1]?.trim() ?? null;

  const planSection = text.split(/^## Flight plan\s*$/m)[1];
  if (planSection) {
    for (const m of planSection.matchAll(/^###\s+(.+)$/gm)) {
      out.taskTitles.push(m[1].replace(TASK_NUM, "").trim());
    }
  }
  return out;
}

// ── flight assembly ───────────────────────────────────────────────────

function deriveStatus(tasks: FlightTask[], review: MorningReview | null): FlightStatus {
  const blocked = review?.blockedOn?.trim().toLowerCase() ?? "none";
  if (blocked && blocked !== "none" && blocked !== "none." && blocked !== "—")
    return "needs_you";
  if (tasks.length === 0) return "ready";
  if (tasks.every((t) => t.status === "done")) return "complete";
  if (tasks.some((t) => t.status === "in_flight" || t.status === "done"))
    return "in_flight";
  return "ready";
}

function launchCommandFor(projectPath: string): string {
  return `cd ${projectPath} && claude --agent supervisor`;
}

function readReviewed(projectPath: string): Record<string, string> {
  try {
    const p = join(projectPath, ".quorum", "autopilot", "reviewed.json");
    return JSON.parse(readFileSync(p, "utf-8"));
  } catch {
    return {};
  }
}

export function setReviewed(projectPath: string, ids: string[], reviewed: boolean): void {
  const dir = join(projectPath, ".quorum", "autopilot");
  mkdirSync(dir, { recursive: true });
  const flags = readReviewed(projectPath);
  for (const id of ids) {
    if (reviewed) flags[id] = new Date().toISOString();
    else delete flags[id];
  }
  writeFileSync(join(dir, "reviewed.json"), JSON.stringify(flags, null, 2) + "\n");
}

function flightFromCheckpoint(
  id: string,
  source: Flight["source"],
  projectPath: string,
  text: string,
  reviewedFlags: Record<string, string>,
  planInfo?: ParsedPlan | null,
): Flight {
  const cp = parseCheckpoint(text);
  const name =
    cp.goal ??
    planInfo?.goal ??
    cp.tasks[0]?.title ??
    planInfo?.taskTitles[0] ??
    `Flight — ${basename(projectPath)}`;

  // A scaffolded checkpoint has no tasks yet; show the plan's tasks as pending
  // so a freshly generated flight reads as "ready" with its plan visible.
  const tasks =
    cp.tasks.length > 0
      ? cp.tasks
      : (planInfo?.taskTitles ?? []).map((t, i) => ({
          index: i + 1,
          title: t,
          status: "pending" as FlightTaskStatus,
          warn: false,
        }));

  return {
    id,
    source,
    fixture: cp.fixture,
    project: projectPath,
    name,
    mode: cp.mode ?? planInfo?.mode ?? null,
    specVersion: cp.specVersion,
    createdAt: cp.createdAt,
    updatedAt: cp.updatedAt,
    status: deriveStatus(tasks, cp.morningReview),
    tasks,
    outcomes: cp.outcomes,
    morningReview: cp.morningReview,
    reviewed: id in reviewedFlags,
    launchCommand: launchCommandFor(projectPath),
  };
}

// Bundled fixture directory (committed with quorum-web) — the fallback when a
// project has no .quorum/autopilot/ data at all, so the morning-review UI is
// demonstrable on any project. Files are the same checkpoint schema.
const BUNDLED_FIXTURES_DIR = resolve(import.meta.dir, "fixtures");

export function buildFlights(projectPath: string): Flight[] {
  const reviewedFlags = readReviewed(projectPath);
  const autopilotDir = join(projectPath, ".quorum", "autopilot");
  const flights: Flight[] = [];

  // Live flight — checkpoint.md + SUPERVISOR.md
  const checkpointPath = join(autopilotDir, "checkpoint.md");
  let planInfo: ParsedPlan | null = null;
  const supervisorPath = join(projectPath, "SUPERVISOR.md");
  if (existsSync(supervisorPath)) {
    try {
      planInfo = parseSupervisorMd(readFileSync(supervisorPath, "utf-8"));
    } catch {}
  }
  if (existsSync(checkpointPath)) {
    try {
      flights.push(
        flightFromCheckpoint(
          "current",
          "checkpoint",
          projectPath,
          readFileSync(checkpointPath, "utf-8"),
          reviewedFlags,
          planInfo,
        ),
      );
    } catch {}
  }

  // Archived flights — .quorum/autopilot/archive/*.md (newest first by stem)
  const archiveDir = join(autopilotDir, "archive");
  if (existsSync(archiveDir)) {
    const files = readdirSync(archiveDir)
      .filter((f) => f.endsWith(".md"))
      .sort()
      .reverse();
    for (const f of files) {
      try {
        flights.push(
          flightFromCheckpoint(
            f.replace(/\.md$/, ""),
            "archive",
            projectPath,
            readFileSync(join(archiveDir, f), "utf-8"),
            reviewedFlags,
          ),
        );
      } catch {}
    }
  }

  // Fallback: no autopilot data anywhere → bundled fixtures, so the board is
  // demonstrable. Clearly badged (Fixture: true inside the files).
  if (flights.length === 0 && existsSync(BUNDLED_FIXTURES_DIR)) {
    for (const f of readdirSync(BUNDLED_FIXTURES_DIR).filter((x) => x.endsWith(".md")).sort().reverse()) {
      try {
        flights.push(
          flightFromCheckpoint(
            f.replace(/\.md$/, ""),
            "bundled-fixture",
            projectPath,
            readFileSync(join(BUNDLED_FIXTURES_DIR, f), "utf-8"),
            reviewedFlags,
          ),
        );
      } catch {}
    }
  }

  return flights;
}

// ── SUPERVISOR.md generation (mirrors quorum-core supervisor_init.h) ──
//
// Same canonical section order + wording as the C++ generator so the
// supervisor SKILL's startup gate reads either output identically. The web
// composer differs only in: generated_by, a goal/mode frontmatter + Project
// lines, and a REAL flight plan instead of the placeholder task.

function listRosterRows(projectPath: string): { name: string; role: string; skill: string }[] {
  const agentsDir = join(projectPath, ".quorum", "agents");
  if (!existsSync(agentsDir)) return [];
  const rows: { name: string; role: string; skill: string }[] = [];
  const files = readdirSync(agentsDir).filter((f) => f.endsWith(".yaml")).sort();
  for (const f of files) {
    const text = readFileSync(join(agentsDir, f), "utf-8");
    const role = text.match(/^role:\s*(.+)$/m)?.[1]?.trim() ?? "";
    const skill = text.match(/^skill_file:\s*(.+)$/m)?.[1]?.trim() ?? "";
    rows.push({ name: f.replace(/\.yaml$/, ""), role, skill });
  }
  return rows;
}

export function generateSupervisorMd(projectPath: string, plan: PlanPayload): string {
  const projectName = basename(projectPath.replace(/\/+$/, ""));
  const oneLineGoal = plan.goal.replace(/\s*\n\s*/g, " ").trim();
  let out = "";

  out += "---\n";
  out += "title: Autopilot flight plan\n";
  out += "generated_by: quorum-web autopilot composer\n";
  out += "spec_version: 0.4\n";
  out += `project_root: ${projectPath}\n`;
  out += `mode: ${plan.mode}\n`;
  out += `goal: ${oneLineGoal}\n`;
  out += "---\n\n";

  out += "# SUPERVISOR.md — Autopilot Flight Plan\n\n";

  out += "## Project\n\n";
  out += `- name: ${projectName}\n`;
  out += `- root: ${projectPath}\n`;
  out +=
    plan.mode === "brainstorm"
      ? "- mode: brainstorm — READ-ONLY flight: slices explore and analyze only; no project file mutations\n"
      : "- mode: generic — subagents may modify the project\n";
  out += `- goal: ${oneLineGoal}\n\n`;

  out += "## Roster (subagent workers)\n\n";
  out += "| agent | role | skill |\n";
  out += "|-------|------|-------|\n";
  const roster = listRosterRows(projectPath);
  if (roster.length === 0) {
    out += "(no agents configured — run `quorum agent create` first)\n";
  } else {
    for (const r of roster) {
      out += `| ${r.name} | ${r.role || "—"} | ${r.skill || "—"} |\n`;
    }
  }
  out += "\n";

  out += "## Record-keeping (knower refresh — end of flight)\n\n";
  out += "- The knowers are the sole accumulators. There is no scribe and no learnings.md.\n";
  out += "- At end-of-flight, refresh the affected knowers so their surveys re-survey the changed code:\n";
  out += `  \`quorum knower refresh --project ${projectPath} --all\`\n`;
  out += "  (or a single lens: `--knower <cartographer|architect|historian|recap>`)\n\n";
  out += "Humans read project state on demand via `quorum ask` (knower surveys + live code) or\n";
  out += "`quorum ask --agent recap`. There is no separate curated layer to maintain.\n\n";

  // Git discipline (findings F1/F6/F4) — fixed section, byte-parallel with the
  // CLI generator (quorum-core/src/cli/supervisor_init.h). Keep the two in sync.
  out += "## Git discipline\n\n";
  out +=
    "- Commit each completed major task BEFORE advancing, staging ONLY " +
    "the paths that task touched: `git add <paths>` then " +
    '`git commit -m "Task N: <title>"`. Never `git add -A` / `git add ' +
    ".` — a shared working tree may hold another writer's in-flight " +
    "work.\n";
  out +=
    "- No external git in this repo while the supervisor runs: it holds " +
    "the working tree via `.quorum/autopilot/LOCK` (written at startup, " +
    "removed on every graceful stop). Operators review + commit only " +
    "after it stops.\n\n";

  out += "## Stop conditions\n\n";
  out += "- context_near_full: checkpoint + write morning review + STOP\n";
  out += "- window_exhausted: STOP at the window edge\n";
  out += "- needs_human: STOP, leave the question in the morning review\n";
  out += `- max_major_tasks: ${plan.maxMajorTasks && plan.maxMajorTasks > 0 ? plan.maxMajorTasks : "—"}\n\n`;

  out += "## Flight plan\n";
  plan.tasks.forEach((t, i) => {
    out += `\n### Task ${i + 1}: ${t.title.trim()}\n`;
    out += `- agent: ${t.agent.trim()}\n`;
    out += "- slices (parallel):\n";
    const slices = t.slices.map((s) => s.trim()).filter(Boolean);
    for (const s of slices.length > 0 ? slices : [t.title.trim()]) {
      out += `  - ${s.replace(/\s*\n\s*/g, " ")}\n`;
    }
    out += `- done when: ${(t.doneWhen.trim() || "the slices above are complete and verified").replace(/\s*\n\s*/g, " ")}\n`;
  });

  return out;
}

function utcNowIso(): string {
  return new Date().toISOString().replace(/\.\d{3}Z$/, "Z");
}

// Matches quorum-core render_checkpoint_skeleton (supervisor populates tasks
// from SUPERVISOR.md on first run).
export function renderCheckpointSkeleton(utc: string): string {
  let out = "";
  out += "# Autopilot checkpoint\n\n";
  out += `Created at: ${utc}\n`;
  out += `Updated at: ${utc}\n`;
  out += "Flight spec: 0.3\n\n";
  out += "## Major tasks\n\n";
  out += "(populated from SUPERVISOR.md on first run)\n\n";
  out += "## Condensed outcomes\n\n";
  out += "(populated after each major task)\n\n";
  out += "## Morning review\n\n";
  out += "- done: none yet\n";
  out += "- pending: (populated on first run)\n";
  out += "- blocked-on: none\n";
  return out;
}

export interface WritePlanResult {
  success: boolean;
  needsForce?: boolean;
  error?: string;
  path?: string;
  content?: string;
  launchCommand?: string;
  archivedPrevious?: string | null;
}

// Write SUPERVISOR.md + scaffold the checkpoint. Semantics mirror the C++
// generator's foolproofing, extended for the web flow:
//   - existing SUPERVISOR.md → needsForce unless force (never silently clobber)
//   - existing checkpoint, flight COMPLETE (all [x], or skeleton) → archive it
//     to .quorum/autopilot/archive/<date>-<slug>.md, scaffold fresh
//   - existing checkpoint, flight MID-RUN ([>] / partial) → refuse unless
//     force (a flight is in progress or awaiting terminal resume)
export function writePlan(projectPath: string, plan: PlanPayload): WritePlanResult {
  if (!existsSync(join(projectPath, ".quorum"))) {
    return { success: false, error: `No .quorum/ at ${projectPath} — run quorum init first` };
  }
  if (!plan.goal?.trim()) return { success: false, error: "goal is required" };
  if (!Array.isArray(plan.tasks) || plan.tasks.length === 0)
    return { success: false, error: "at least one major task is required" };
  for (const t of plan.tasks) {
    if (!t.title?.trim()) return { success: false, error: "every task needs a title" };
    if (!t.agent?.trim()) return { success: false, error: `task "${t.title}" needs a roster agent` };
  }

  const supervisorPath = join(projectPath, "SUPERVISOR.md");
  const autopilotDir = join(projectPath, ".quorum", "autopilot");
  const checkpointPath = join(autopilotDir, "checkpoint.md");

  const checkpointExists = existsSync(checkpointPath);
  let checkpointState: "absent" | "fresh" | "complete" | "mid_run" = "absent";
  let oldCheckpoint: ParsedCheckpoint | null = null;
  if (checkpointExists) {
    oldCheckpoint = parseCheckpoint(readFileSync(checkpointPath, "utf-8"));
    if (oldCheckpoint.tasks.length === 0) checkpointState = "fresh";
    else if (oldCheckpoint.tasks.every((t) => t.status === "done")) checkpointState = "complete";
    else checkpointState = "mid_run";
  }

  if (!plan.force && existsSync(supervisorPath)) {
    return {
      success: false,
      needsForce: true,
      error:
        checkpointState === "mid_run"
          ? "A flight is mid-run (checkpoint has unfinished tasks) — overwriting the plan orphans its resume state."
          : "SUPERVISOR.md already exists — overwrite with the new flight plan?",
    };
  }
  if (!plan.force && checkpointState === "mid_run") {
    return {
      success: false,
      needsForce: true,
      error: "A flight is mid-run (checkpoint has unfinished tasks). Force to archive it and start fresh.",
    };
  }

  // Archive a completed (or force-abandoned mid-run) checkpoint before
  // scaffolding fresh — this is how past flights accumulate for review.
  let archivedPrevious: string | null = null;
  if (checkpointExists && checkpointState !== "fresh") {
    const archiveDir = join(autopilotDir, "archive");
    mkdirSync(archiveDir, { recursive: true });
    const date = (oldCheckpoint?.updatedAt ?? utcNowIso()).slice(0, 10);
    const slugSource = oldCheckpoint?.goal ?? oldCheckpoint?.tasks[0]?.title ?? "flight";
    const slug =
      slugSource.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 48) || "flight";
    let dest = join(archiveDir, `${date}-${slug}.md`);
    for (let n = 2; existsSync(dest); n++) dest = join(archiveDir, `${date}-${slug}-${n}.md`);
    renameSync(checkpointPath, dest);
    archivedPrevious = basename(dest);
  }

  const content = generateSupervisorMd(projectPath, plan);
  writeFileSync(supervisorPath, content);

  if (!existsSync(checkpointPath)) {
    mkdirSync(autopilotDir, { recursive: true });
    writeFileSync(checkpointPath, renderCheckpointSkeleton(utcNowIso()));
  }

  return {
    success: true,
    path: supervisorPath,
    content,
    launchCommand: launchCommandFor(projectPath),
    archivedPrevious,
  };
}
