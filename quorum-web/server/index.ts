import { Hono } from "hono";
import { cors } from "hono/cors";
import { config, repoRoot, getState, setCurrentProject, getProjectConfig } from "../config";
import { join, resolve } from "path";
import { homedir } from "os";
import { readFileSync, writeFileSync, readdirSync, existsSync, mkdirSync, copyFileSync, chmodSync } from "fs";

// Expand a leading `~` to the user's home directory and resolve to an
// absolute path. Plain `existsSync(join(p, ".quorum"))` does NOT do shell
// tilde expansion — when the UI passes "~/nonvis/quorum/sample" the
// lookup would otherwise hit "./~/nonvis/quorum/sample/.quorum" and fail.
function expandPath(p: string): string {
  if (!p) return p;
  let expanded = p;
  if (expanded === "~") {
    expanded = homedir();
  } else if (expanded.startsWith("~/")) {
    expanded = homedir() + expanded.slice(1);
  }
  return resolve(expanded);
}
import {
  getConversations,
  getConversation,
  getTasksForConversation,
  getStats,
  freshQuery,
  dbWrite,
  type Conversation,
} from "./db";
import { execDaemon, execDaemonAt, execAsk, spawnDaemon, cleanupStaleDaemon, isDaemonRunning } from "./daemon";
import { createSSEStream } from "./sse";

const app = new Hono();

// CORS for local dev (Vite runs on different port)
app.use("*", cors());

// -- Project endpoints --

app.get("/api/projects", (c) => {
  const state = getState();
  return c.json({ current: state.currentProject, recent: state.recentProjects });
});

app.post("/api/projects/select", async (c) => {
  const body = await c.req.json<{ path: string }>();
  const projectPath = expandPath(body.path);
  const quorumDir = join(projectPath, ".quorum");
  if (!existsSync(quorumDir)) {
    return c.json({ error: `No .quorum/ directory found in ${projectPath}` }, 400);
  }
  setCurrentProject(projectPath);
  return c.json({ success: true, path: projectPath });
});

app.post("/api/init", async (c) => {
  const body = await c.req.json<{ path: string }>();
  if (!body.path?.trim()) {
    return c.json({ error: "path is required" }, 400);
  }
  const projectPath = expandPath(body.path);
  const quorumDir = join(projectPath, ".quorum");
  if (existsSync(quorumDir)) {
    return c.json({ error: `Project already initialized: ${quorumDir} exists` }, 400);
  }
  const result = await execDaemonAt(projectPath, "init");
  if (result.success) {
    setCurrentProject(projectPath);
    return c.json({ success: true, output: result.stdout });
  }
  return c.json({ success: false, error: result.stderr || result.stdout }, 500);
});

// -- Agent endpoints --

app.get("/api/agents", (c) => {
  const state = getState();
  if (!state.currentProject) return c.json([]);

  const agentsDir = join(state.currentProject, ".quorum", "agents");
  if (!existsSync(agentsDir)) return c.json([]);

  const files = readdirSync(agentsDir).filter(
    (f) => f.endsWith(".yaml") || f.endsWith(".yml")
  );

  const agents = files.map((file) => {
    const content = readFileSync(join(agentsDir, file), "utf-8");
    const stem = file.replace(/\.ya?ml$/, "");
    const id = content.match(/^id:\s*(.+)/m)?.[1]?.trim() ?? stem;
    const rawName = content.match(/^name:\s*(.+)/m)?.[1]?.trim() ?? id;
    const name = rawName.replace(/^["']|["']$/g, "");
    const role = content.match(/^role:\s*(.+)/m)?.[1]?.trim() ?? "";
    const rawDesc = content.match(/^description:\s*(.+)/m)?.[1]?.trim() ?? "";
    const description = rawDesc.replace(/^["']|["']$/g, "");
    const skill = content.match(/^skill_file:\s*(.+)/m)?.[1]?.trim() ?? null;
    return { id, name, role, description, skill_file: skill };
  });

  return c.json(agents);
});

// Knower "specialties" — read-only `thinker` agents pointed at a knower SKILL,
// meant to run in `--mode brainstorm` (clamped to Read/Grep/Glob). These are NOT
// distinct roles, so the daemon has no `--role cartographer`; the web UI offers
// them as a separate "specialty" path that resolves to thinker + the canonical
// SKILL + description, then runs the deterministic Tier-1 scan.
//
// This mirrors scripts/setup-knowers.sh, which is the sibling source of truth for
// the descriptions and the per-knower Tier-1 wiring. Skill paths + Tier-1 tool
// scripts are resolved against the quorum repo root (repoRoot). Keep in sync.
const KNOWERS: Record<string, {
  skill: string;        // SKILL.md, relative to repoRoot
  description: string;  // canonical agent description (matches setup-knowers.sh)
  tier1?: { tool: string; gate: "always" | "gh" }; // deterministic, zero-token scan
}> = {
  cartographer: {
    skill: "templates/skills/cartographer/SKILL.md",
    description: "Cartographer: knows the project layout. Reads the Tier-1 index (.quorum/cartographer/layout.json) + honors the root CLAUDE.md; produces a fast-lookup project index. Read-only.",
    tier1: { tool: "scripts/cartographer_index.py", gate: "always" },
  },
  architect: {
    skill: "templates/skills/architect/SKILL.md",
    description: "Architect: maps component interconnections (imports, cross-repo calls, event flows) with file evidence; traces the primary flow; flags coupling/invariants. Read-only.",
  },
  historian: {
    skill: "templates/skills/historian/SKILL.md",
    description: "Historian: knows the project's decisions + pivots. Reads the Tier-1 record (.quorum/historian/decisions-raw.json) + the Decision Log; tracks status/supersession with PR/commit provenance. Read-only.",
    tier1: { tool: "scripts/historian_mine.py", gate: "gh" },
  },
  recap: {
    skill: "templates/skills/recap/SKILL.md",
    description: "Recap: knows what changed recently + where you left off (WHAT/WHEN). Reads the Tier-1 windowed timeline (.quorum/recap/timeline-raw.json) + operator-dumped timestamped messages, weaves one dated component-grouped timeline, drafts where-i-left-off, with a by-intent read-only Linear status overlay. Read-only; never queries Linear/Slack/Telegram.",
    tier1: { tool: "scripts/recap_mine.py", gate: "always" },
  },
};

// Is `gh` installed AND authenticated? (historian's Tier-1 mine shells out to it.)
async function ghAuthed(): Promise<boolean> {
  try {
    const proc = Bun.spawn(["gh", "auth", "token"], { stdout: "pipe", stderr: "pipe" });
    const out = (await new Response(proc.stdout).text()).trim();
    const code = await proc.exited;
    return code === 0 && out.length > 0;
  } catch {
    return false; // gh not on PATH
  }
}

// Install a knower's deterministic Tier-1 tool into <project>/.quorum/tools/ and
// run its scan (read-only, zero tokens) so the knower has its index to read.
// Mirrors setup-knowers.sh: cartographer/recap run unconditionally; historian's
// mine is gh-gated and skipped (with a note) when gh is missing/unauthenticated.
async function runKnowerTier1(projectPath: string, tier1: { tool: string; gate: "always" | "gh" }): Promise<string> {
  const src = resolve(repoRoot, tier1.tool);
  const base = tier1.tool.split("/").pop()!;
  if (!existsSync(src)) return `Tier-1 tool missing in repo: ${tier1.tool} (skipped)`;

  const toolsDir = join(projectPath, ".quorum", "tools");
  mkdirSync(toolsDir, { recursive: true });
  const dest = join(toolsDir, base);
  copyFileSync(src, dest);
  try { chmodSync(dest, 0o755); } catch {}

  if (tier1.gate === "gh" && !(await ghAuthed())) {
    return `Installed ${base}; skipped its mine (gh not authenticated). Run later: python3 .quorum/tools/${base} --root .`;
  }

  const proc = Bun.spawn(["python3", dest, "--root", projectPath], {
    cwd: projectPath, stdout: "pipe", stderr: "pipe",
  });
  const stderr = (await new Response(proc.stderr).text()).trim();
  const code = await proc.exited;
  if (code !== 0) return `Installed ${base}; Tier-1 scan exited ${code}${stderr ? ": " + stderr.split("\n").pop() : ""}`;
  return `Ran deterministic Tier-1 scan (${base}).`;
}

app.post("/api/agents", async (c) => {
  const body = await c.req.json<{
    role?: string;
    specialty?: string;
    name?: string;
    description?: string;
    targetDir?: string;
    skill?: string;
  }>();
  if (!body.name) {
    return c.json({ error: "name is required" }, 400);
  }

  // Knower-specialty path: resolve to thinker + canonical SKILL + description.
  const knower = body.specialty ? KNOWERS[body.specialty] : undefined;
  if (body.specialty && !knower) {
    return c.json({ error: `Unknown specialty: ${body.specialty}` }, 400);
  }

  let projectPath: string | null = null;
  let role = body.role;
  let description = body.description;
  let skillFileAbs: string | undefined;

  if (knower) {
    const state = getState();
    if (!state.currentProject) {
      return c.json({ error: "Select a project before adding a knower specialty" }, 400);
    }
    projectPath = getProjectConfig(state.currentProject).projectPath;
    role = "thinker";
    skillFileAbs = resolve(repoRoot, knower.skill);
    if (!existsSync(skillFileAbs)) {
      return c.json({ error: `Knower skill not found: ${skillFileAbs}` }, 500);
    }
    if (!description) description = knower.description;
  }

  if (!role) {
    return c.json({ error: "role (or specialty) is required" }, 400);
  }

  const args = ["agent", "create", "--role", role, "--name", body.name, "--no-ai"];
  if (description) args.push("--description", description);
  if (skillFileAbs) {
    // Knower: absolute SKILL path + project as target-dir (parity w/ setup-knowers.sh)
    args.push("--skill-file", skillFileAbs);
    if (projectPath) args.push("--target-dir", projectPath);
  } else {
    if (body.targetDir) args.push("--target-dir", body.targetDir);
    if (body.skill) args.push("--skill", body.skill);
  }

  const result = await execDaemon(...args);

  // For knowers with a deterministic Tier-1 tool, install + run it after the
  // agent yaml is written, so the knower is immediately usable.
  let tier1Note: string | undefined;
  if (result.success && knower?.tier1 && projectPath) {
    tier1Note = await runKnowerTier1(projectPath, knower.tier1);
  }

  return c.json({
    success: result.success,
    output: [result.stdout, tier1Note].filter(Boolean).join("\n"),
    error: result.stderr || undefined,
  });
});

app.get("/api/agents/:id/context", (c) => {
  const id = c.req.param("id");
  const state = getState();
  if (!state.currentProject) return c.json({ error: "No project selected" }, 400);
  const contextPath = join(state.currentProject, ".quorum", "vaults", id, "CONTEXT.md");
  if (!existsSync(contextPath)) return c.json({ error: "CONTEXT.md not found", content: "" }, 404);
  const content = readFileSync(contextPath, "utf-8");
  return c.json({ id, content });
});

// Audit-trail helper for CONTEXT.md rewrites. Mirrors the C++
// vault/context_history.h logic: same `---QUORUM-HISTORY---` sentinel,
// same ISO8601 UTC format, same 20-entry cap. See that header for
// rationale on the sentinel choice (markdown horizontal rules / YAML
// frontmatter delimiters can't collide with this sentinel).
const HISTORY_SENTINEL = "---QUORUM-HISTORY---";
const HISTORY_MAX_ENTRIES = 20;

function historyTimestampIso(): string {
  return new Date().toISOString().replace(/\.\d{3}Z$/, "Z");
}

function splitHistoryRecords(content: string): string[] {
  if (!content) return [];
  const lines = content.split("\n");
  const records: string[] = [];
  let inRecord = false;
  let current = "";
  let firstLine = true;
  for (const line of lines) {
    if (line === HISTORY_SENTINEL) {
      if (inRecord) records.push(current);
      current = "";
      inRecord = true;
      firstLine = false;
    } else if (inRecord) {
      current += (firstLine ? "" : "") + line + "\n";
      firstLine = false;
    }
  }
  if (inRecord) records.push(current);
  return records;
}

function writeContextWithHistory(contextPath: string, newContent: string): void {
  if (existsSync(contextPath)) {
    const prior = readFileSync(contextPath, "utf-8");
    const historyPath = contextPath + ".history";
    const ts = historyTimestampIso();
    let record = `## ${ts}\n\n${prior}`;
    if (!record.endsWith("\n")) record += "\n";

    const existing = existsSync(historyPath) ? readFileSync(historyPath, "utf-8") : "";
    const records = splitHistoryRecords(existing);
    records.push(record);

    const trimmed = records.length > HISTORY_MAX_ENTRIES
      ? records.slice(records.length - HISTORY_MAX_ENTRIES)
      : records;

    const reassembled = trimmed.map((r) => `${HISTORY_SENTINEL}\n${r}`).join("");
    writeFileSync(historyPath, reassembled);
  }
  writeFileSync(contextPath, newContent);
}

app.put("/api/agents/:id/context", async (c) => {
  const id = c.req.param("id");
  const body = await c.req.json<{ content: string }>();
  const state = getState();
  if (!state.currentProject) return c.json({ error: "No project selected" }, 400);
  const vaultDir = join(state.currentProject, ".quorum", "vaults", id);
  if (!existsSync(vaultDir)) {
    mkdirSync(vaultDir, { recursive: true });
  }
  const contextPath = join(vaultDir, "CONTEXT.md");
  writeContextWithHistory(contextPath, body.content);
  return c.json({ success: true });
});

// -- Daemon status --

app.get("/api/daemon/status", (c) => {
  return c.json({ running: isDaemonRunning() });
});

// -- Read endpoints --

app.get("/api/conversations", (c) => {
  return c.json(getConversations());
});

app.get("/api/conversations/:id", (c) => {
  const id = Number(c.req.param("id"));
  const conv = getConversation(id);
  if (!conv) return c.json({ error: "not found" }, 404);
  const tasks = getTasksForConversation(id);
  return c.json({ ...conv, tasks });
});

app.get("/api/conversations/:id/tasks", (c) => {
  const id = Number(c.req.param("id"));
  return c.json(getTasksForConversation(id));
});

app.get("/api/stats", (c) => {
  return c.json(getStats());
});

app.get("/api/budget", (c) => {
  try {
    const rows = freshQuery<{
      budget_usd: number;
      window_hours: number;
      window_start: string;
      spent_usd: number;
    }>("SELECT budget_usd, window_hours, window_start, spent_usd FROM budget_window WHERE id = 1");

    if (rows.length === 0) {
      return c.json({
        budget_usd: 100, window_hours: 5, window_start: null,
        spent_usd: 0, remaining_usd: 100, remaining_minutes: 300, is_expired: false,
      });
    }

    const row = rows[0];
    const start = new Date(row.window_start + "Z"); // SQLite stores UTC without Z
    const endMs = start.getTime() + row.window_hours * 3600 * 1000;
    const remainingMs = Math.max(0, endMs - Date.now());
    const remainingMinutes = Math.floor(remainingMs / 60000);

    return c.json({
      budget_usd: row.budget_usd,
      window_hours: row.window_hours,
      window_start: row.window_start,
      spent_usd: Math.round(row.spent_usd * 100) / 100,
      remaining_usd: Math.round((row.budget_usd - row.spent_usd) * 100) / 100,
      remaining_minutes: remainingMinutes,
      is_expired: remainingMs <= 0,
    });
  } catch {
    return c.json({
      budget_usd: 100, window_hours: 5, window_start: null,
      spent_usd: 0, remaining_usd: 100, remaining_minutes: 300, is_expired: false,
    });
  }
});

app.post("/api/budget", async (c) => {
  const body = await c.req.json<{
    budget_usd?: number;
    remaining_minutes?: number;
  }>();

  if (body.budget_usd != null && body.budget_usd > 0) {
    dbWrite("UPDATE budget_window SET budget_usd = ? WHERE id = 1", [body.budget_usd]);
  }

  if (body.remaining_minutes != null && body.remaining_minutes >= 0) {
    // Compute window_start so that window_start + window_hours = now + remaining_minutes
    const rows = freshQuery<{ window_hours: number }>(
      "SELECT window_hours FROM budget_window WHERE id = 1"
    );
    const windowHours = rows[0]?.window_hours ?? 5;
    const windowStartMs = Date.now() + body.remaining_minutes * 60000 - windowHours * 3600000;
    const windowStart = new Date(windowStartMs).toISOString()
      .replace("T", " ").split(".")[0];
    dbWrite(
      "UPDATE budget_window SET window_start = ?, spent_usd = 0 WHERE id = 1",
      [windowStart]
    );
  }

  return c.json({ success: true });
});

app.get("/api/budget/agents", (c) => {
  try {
    // Phase 7 Track 5 — surface Anthropic prompt-cache totals + hit ratio.
    // cache_hit_ratio = cache_read / (cache_read + cache_creation + input)
    // — i.e. share of input-side tokens served from cache. Coalesce nulls
    // to 0 so legacy rows (pre-Track-5 columns) don't poison the math.
    //
    // Phase 8 Track 3 — avg_score column. Average of the last N (default 10)
    // evaluations where this agent was the scored_agent_id. NULL when this
    // agent has no evaluations on record (e.g. cleanly missing on dashboards
    // with no scoring activity).
    const N = 10;
    const rows = freshQuery<{
      agent: string;
      tasks: number;
      total_cost: number;
      avg_cost: number;
      cache_read_tokens: number;
      cache_creation_tokens: number;
      input_tokens: number;
      cache_hit_ratio: number;
      avg_score: number | null;
    }>(
      "SELECT t.agent as agent, COUNT(*) as tasks, " +
      "ROUND(COALESCE(SUM(t.cost), 0), 2) as total_cost, " +
      "ROUND(COALESCE(AVG(t.cost), 0), 2) as avg_cost, " +
      "COALESCE(SUM(t.cache_read_input_tokens), 0) as cache_read_tokens, " +
      "COALESCE(SUM(t.cache_creation_input_tokens), 0) as cache_creation_tokens, " +
      "COALESCE(SUM(t.token_in), 0) as input_tokens, " +
      "CASE WHEN " +
      "  (COALESCE(SUM(t.cache_read_input_tokens), 0) + " +
      "   COALESCE(SUM(t.cache_creation_input_tokens), 0) + " +
      "   COALESCE(SUM(t.token_in), 0)) > 0 " +
      "THEN ROUND(" +
      "  CAST(COALESCE(SUM(t.cache_read_input_tokens), 0) AS REAL) / " +
      "  (COALESCE(SUM(t.cache_read_input_tokens), 0) + " +
      "   COALESCE(SUM(t.cache_creation_input_tokens), 0) + " +
      "   COALESCE(SUM(t.token_in), 0)), 4) " +
      "ELSE 0 END as cache_hit_ratio, " +
      "COALESCE((SELECT ROUND(AVG(score_total), 2) FROM ( " +
      "  SELECT score_total FROM evaluations " +
      "  WHERE scored_agent_id = t.agent " +
      "  ORDER BY created_at DESC, id DESC LIMIT " + N + " " +
      ")), NULL) as avg_score " +
      "FROM tasks t WHERE t.cost > 0 GROUP BY t.agent ORDER BY total_cost DESC"
    );
    return c.json(rows);
  } catch {
    return c.json([]);
  }
});

// Phase 8 Track 3 — recent evaluations for a scored agent.
// `:id` matches `evaluations.scored_agent_id`. Returns up to ?limit= rows
// (default 20) ordered by recency. Omits score_json for compactness; use
// the per-id detail endpoint for the full breakdown.
app.get("/api/evaluations/agent/:id", (c) => {
  try {
    const id = c.req.param("id");
    const limitParam = c.req.query("limit");
    let limit = 20;
    const parsed = limitParam ? parseInt(limitParam, 10) : NaN;
    if (Number.isFinite(parsed) && parsed > 0 && parsed <= 200) {
      limit = parsed;
    }
    const rows = freshQuery<{
      id: number;
      conversation_id: number;
      evaluator_agent_id: string;
      role_specialty: string;
      rubric_version: string;
      score_total: number;
      notes: string | null;
      created_at: string;
    }>(
      "SELECT id, conversation_id, evaluator_agent_id, role_specialty, " +
      "rubric_version, score_total, notes, created_at " +
      "FROM evaluations WHERE scored_agent_id = ? " +
      "ORDER BY created_at DESC, id DESC LIMIT ?",
      [id, limit]
    );
    return c.json(rows);
  } catch {
    return c.json([]);
  }
});

// Phase 8 Track 3 — full evaluation detail including score_json. 404 if
// no row matches the id.
app.get("/api/evaluations/:id", (c) => {
  try {
    const id = parseInt(c.req.param("id"), 10);
    if (!Number.isFinite(id) || id <= 0) {
      return c.json({ error: "invalid id" }, 400);
    }
    const rows = freshQuery<{
      id: number;
      conversation_id: number;
      scored_agent_id: string;
      evaluator_agent_id: string;
      role_specialty: string;
      rubric_version: string;
      score_total: number;
      score_json: string;
      notes: string | null;
      created_at: string;
    }>(
      "SELECT id, conversation_id, scored_agent_id, evaluator_agent_id, " +
      "role_specialty, rubric_version, score_total, score_json, notes, " +
      "created_at FROM evaluations WHERE id = ?",
      [id]
    );
    if (rows.length === 0) {
      return c.json({ error: "not found" }, 404);
    }
    return c.json(rows[0]);
  } catch {
    return c.json({ error: "query failed" }, 500);
  }
});

app.get("/api/config", (c) => {
  try {
    const yaml = readFileSync(config.configPath, "utf-8");

    // Parse daemon section
    const targetDir = yaml.match(/^\s+target_dir:\s*(.+)/m)?.[1]?.trim() ?? null;
    const pidFile = yaml.match(/^\s+pid_file:\s*(.+)/m)?.[1]?.trim() ?? null;
    const dataDir = yaml.match(/^\s+data_dir:\s*(.+)/m)?.[1]?.trim() ?? null;
    const logLevel = yaml.match(/^\s+log_level:\s*(.+)/m)?.[1]?.trim() ?? null;

    // Parse budget section
    const windowBudget = parseFloat(yaml.match(/^\s+window_budget_usd:\s*(.+)/m)?.[1] ?? "0") || null;
    const windowHours = parseFloat(yaml.match(/^\s+window_hours:\s*(.+)/m)?.[1] ?? "0") || null;

    // Parse conversations section
    const leader = yaml.match(/^\s+leader:\s*(.+)/m)?.[1]?.trim() ?? null;
    const defaultPath = yaml.match(/^\s+default_path:\s*(.+)/m)?.[1]?.trim() ?? null;
    const convBudget = parseFloat(yaml.match(/^\s+default_budget_usd:\s*(.+)/m)?.[1] ?? "0") || null;
    const maxRounds = parseInt(yaml.match(/^\s+default_max_rounds:\s*(.+)/m)?.[1] ?? "0") || null;

    // Parse agents
    const agentConfigs = [...yaml.matchAll(/^\s+-\s*config:\s*(.+)/gm)].map((m) => m[1].trim());

    return c.json({
      config_path: config.configPath,
      daemon: { target_dir: targetDir, pid_file: pidFile, data_dir: dataDir, log_level: logLevel },
      budget: { window_budget_usd: windowBudget, window_hours: windowHours },
      conversations: { leader, default_path: defaultPath, default_budget_usd: convBudget, default_max_turns: maxRounds },
      agents: agentConfigs,
    });
  } catch {
    return c.json({
      config_path: config.configPath,
      daemon: { target_dir: null, pid_file: null, data_dir: null, log_level: null },
      budget: { window_budget_usd: null, window_hours: null },
      conversations: { leader: null, default_path: null, default_budget_usd: null, default_max_turns: null },
      agents: [],
    });
  }
});

app.post("/api/config", async (c) => {
  const updates = await c.req.json<Record<string, string | number | boolean>>();

  try {
    let yaml = readFileSync(config.configPath, "utf-8");

    // Map of field names to YAML keys
    const fieldMap: Record<string, string> = {
      target_dir: "target_dir",
      log_level: "log_level",
      window_budget_usd: "window_budget_usd",
      window_hours: "window_hours",
      default_budget_usd: "default_budget_usd",
      default_max_turns: "default_max_rounds",
      leader: "leader",
      default_path: "default_path",
    };

    for (const [field, value] of Object.entries(updates)) {
      const yamlKey = fieldMap[field];
      if (!yamlKey) continue;

      // Replace the value in the YAML (preserving indentation)
      const regex = new RegExp(`^(\\s+${yamlKey}:\\s*)(.+)$`, "m");
      if (regex.test(yaml)) {
        yaml = yaml.replace(regex, `$1${value}`);
      }
    }

    writeFileSync(config.configPath, yaml);

    return c.json({ success: true });
  } catch (e) {
    return c.json({ error: "Failed to update config" }, 500);
  }
});

// -- SSE endpoint --

app.get("/api/events", (c) => {
  const stream = createSSEStream();
  return new Response(stream, {
    headers: {
      "Content-Type": "text/event-stream",
      "Cache-Control": "no-cache",
      Connection: "keep-alive",
    },
  });
});

// -- Write endpoints (via daemon CLI) --

app.post("/api/converse", async (c) => {
  const body = await c.req.json<{ goal: string; mode?: string }>();
  if (!body.goal) return c.json({ error: "goal is required" }, 400);

  // Validate mode early — must match what the C++ CLI accepts.
  if (body.mode != null && body.mode !== "generic" && body.mode !== "brainstorm") {
    return c.json({
      error: `mode must be 'generic' or 'brainstorm' (got '${body.mode}')`,
    }, 400);
  }

  // Record current max ID before spawning
  const before = freshQuery<{ max_id: number | null }>(
    "SELECT MAX(id) as max_id FROM conversations"
  );
  const maxIdBefore = before[0]?.max_id ?? 0;

  const modeTag = body.mode ? ` [mode: ${body.mode}]` : "";
  const args: string[] = ["converse"];
  if (body.mode) args.push("--mode", body.mode);
  args.push(body.goal);

  if (isDaemonRunning()) {
    // Daemon already running — just insert conversation via CLI (it exits immediately)
    console.log(`[converse] daemon running, exec: "${body.goal}"${modeTag}`);
    await execDaemon(...args);
  } else {
    // No daemon — spawn one (it creates conversation + runs dispatch loop)
    console.log(`[converse] spawning daemon for: "${body.goal}"${modeTag}`);
    spawnDaemon(...args);
  }

  // Poll for the new conversation (fresh connection each time to bypass WAL snapshot)
  let conversationId: number | null = null;
  for (let attempt = 0; attempt < 10; attempt++) {
    await new Promise((r) => setTimeout(r, 500));
    const rows = freshQuery<Conversation>(
      "SELECT * FROM conversations WHERE id > ? ORDER BY id DESC LIMIT 1",
      [maxIdBefore]
    );
    if (rows.length > 0) {
      conversationId = rows[0].id;
      console.log(`[converse] found conversation ${conversationId} (attempt ${attempt + 1})`);
      break;
    }
  }

  return c.json({
    success: true,
    conversationId,
  });
});

app.post("/api/respond/:id", async (c) => {
  const id = c.req.param("id");
  const body = await c.req.json<{ text: string }>();
  if (!body.text?.trim()) return c.json({ error: "text is required" }, 400);
  const result = await execDaemon("respond", "--conversation", id, body.text);
  return c.json({
    success: result.success,
    output: result.stdout,
    error: result.stderr || undefined,
  });
});

// -- Recap ("What's going on?") -- Phase 14 T6.
//
// Shells `quorum ask --agent recap "<prompt>" --project <currentProject>` (the
// `quorum` binary on PATH is a symlink to daemonBin). The active project is the
// web UI's selected project (getState().currentProject) — the SAME mechanism
// every other project-scoped endpoint uses; no new selection model. The recap
// knower must be set up in the target project (`ask --agent recap` reads
// .quorum/agents/recap.yaml + .quorum/vaults/recap/...); if it isn't, the CLI
// exits non-zero and we surface its stderr so the popup shows the real reason.
app.post("/api/recap", async (c) => {
  const body = await c.req.json<{ prompt: string }>();
  if (!body.prompt?.trim()) return c.json({ error: "prompt is required" }, 400);

  const state = getState();
  if (!state.currentProject) {
    return c.json({ error: "No project selected" }, 400);
  }
  const projectPath = getProjectConfig(state.currentProject).projectPath;

  const result = await execAsk(projectPath, "recap", body.prompt.trim());
  if (!result.success) {
    // Surface the CLI's own message (recap not set up / no answer / timeout)
    // rather than failing silently. Prefer stderr; fall back to stdout.
    return c.json(
      { error: result.stderr || result.stdout || "recap failed" },
      500,
    );
  }

  // `run_ask` prints a one-line "=== <project> recap ===" header before the
  // answer. Strip it so the client renders just the answer body.
  let answer = result.stdout;
  const headerMatch = answer.match(/^===[^\n]*===\n+/);
  if (headerMatch) answer = answer.slice(headerMatch[0].length);

  return c.json({ answer: answer.trim() });
});

app.post("/api/close/:id", async (c) => {
  const id = c.req.param("id");
  const result = await execDaemon("close", "--conversation", id);
  return c.json({
    success: result.success,
    output: result.stdout,
    error: result.stderr || undefined,
  });
});

app.post("/api/resume/:id", async (c) => {
  const id = c.req.param("id");
  // Resume starts the daemon loop — fire and forget like converse
  console.log(`[resume] spawning daemon for conversation ${id}`);
  spawnDaemon("resume", "--conversation", id);
  return c.json({ success: true });
});

app.post("/api/conversations/:id/budget", async (c) => {
  const id = Number(c.req.param("id"));
  const body = await c.req.json<{ budget_usd: number }>();
  if (!body.budget_usd || body.budget_usd <= 0) {
    return c.json({ error: "budget_usd must be positive" }, 400);
  }
  dbWrite("UPDATE conversations SET budget_usd = ? WHERE id = ?", [body.budget_usd, id]);

  // If conversation was paused due to budget, auto-resume
  const conv = freshQuery<{ state: string; paused_reason: string | null }>(
    "SELECT state, paused_reason FROM conversations WHERE id = ?",
    [id]
  );
  if (conv[0]?.state === "paused" && conv[0]?.paused_reason?.includes("budget")) {
    console.log(`[budget] conversation ${id} budget increased to $${body.budget_usd}, auto-resuming`);
    spawnDaemon("resume", "--conversation", String(id));
  }

  return c.json({ success: true, budget_usd: body.budget_usd });
});

// Raise a conversation's max_rounds (the real per-conversation limiter — the
// per-conversation budget is not enforced). Resumes if it was paused (e.g.
// "max turns reached").
app.post("/api/conversations/:id/max-rounds", async (c) => {
  const id = Number(c.req.param("id"));
  const body = await c.req.json<{ max_rounds: number }>();
  if (!body.max_rounds || body.max_rounds <= 0) {
    return c.json({ error: "max_rounds must be positive" }, 400);
  }
  dbWrite("UPDATE conversations SET max_rounds = ? WHERE id = ?", [body.max_rounds, id]);

  const conv = freshQuery<{ state: string }>(
    "SELECT state FROM conversations WHERE id = ?",
    [id]
  );
  if (conv[0]?.state === "paused") {
    console.log(`[max-rounds] conversation ${id} max_rounds -> ${body.max_rounds}, resuming`);
    spawnDaemon("resume", "--conversation", String(id));
  }

  return c.json({ success: true, max_rounds: body.max_rounds });
});

// -- Start --

// Clean up stale daemon processes from previous web server runs
cleanupStaleDaemon();

console.log(`Quorum Web API — http://localhost:${config.port}`);
console.log(`  DB: ${config.dbPath}`);
console.log(`  Daemon: ${config.daemonBin}`);
console.log(`  Config: ${config.configPath}`);
const startupState = getState();
if (startupState.currentProject) console.log(`  Project: ${startupState.currentProject}`);
else console.log(`  Project: (none — select via UI)`);

export default {
  port: config.port,
  fetch: app.fetch,
  idleTimeout: 120, // seconds — prevent premature SSE disconnect
};
