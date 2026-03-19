import { Hono } from "hono";
import { cors } from "hono/cors";
import { config, repoRoot, getState, setCurrentProject, getProjectConfig } from "../config";
import { join } from "path";
import { readFileSync, writeFileSync, readdirSync, existsSync, unlinkSync, mkdirSync } from "fs";
import {
  getConversations,
  getConversation,
  getTasksForConversation,
  getStats,
  freshQuery,
  dbWrite,
  type Conversation,
} from "./db";
import { execDaemon, execDaemonAt, spawnDaemon, cleanupStaleDaemon, isDaemonRunning } from "./daemon";
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
  const quorumDir = join(body.path, ".quorum");
  if (!existsSync(quorumDir)) {
    return c.json({ error: `No .quorum/ directory found in ${body.path}` }, 400);
  }
  setCurrentProject(body.path);
  return c.json({ success: true, path: body.path });
});

app.post("/api/init", async (c) => {
  const body = await c.req.json<{ path: string }>();
  if (!body.path?.trim()) {
    return c.json({ error: "path is required" }, 400);
  }
  const quorumDir = join(body.path, ".quorum");
  if (existsSync(quorumDir)) {
    return c.json({ error: `Project already initialized: ${quorumDir} exists` }, 400);
  }
  const result = await execDaemonAt(body.path, "init");
  if (result.success) {
    setCurrentProject(body.path);
    return c.json({ success: true, output: result.stdout });
  }
  return c.json({ success: false, error: result.stderr || result.stdout }, 500);
});

// -- Team endpoints --

app.get("/api/teams", (c) => {
  const state = getState();
  if (!state.currentProject) return c.json([]);

  const teamsDir = join(state.currentProject, ".quorum", "teams");
  if (!existsSync(teamsDir)) return c.json([]);

  const files = readdirSync(teamsDir).filter(
    (f) => f.endsWith(".yaml") || f.endsWith(".yml")
  );

  const teams = files.map((file) => {
    const content = readFileSync(join(teamsDir, file), "utf-8");
    const stem = file.replace(/\.ya?ml$/, "");
    const name = content.match(/^name:\s*(.+)/m)?.[1]?.trim() ?? stem;
    const pathMatch = content.match(/^default_path:\s*\[(.+)\]/m);
    const default_path = pathMatch
      ? pathMatch[1].split(",").map((s) => s.trim())
      : [];
    return { id: stem, name, default_path };
  });

  return c.json(teams);
});

app.post("/api/teams", async (c) => {
  const body = await c.req.json<{ name: string; defaultPath: string[] }>();
  if (!body.name?.trim() || !body.defaultPath?.length) {
    return c.json({ error: "name and defaultPath are required" }, 400);
  }
  const state = getState();
  if (!state.currentProject) return c.json({ error: "no project selected" }, 400);

  const id = body.name.toLowerCase().replace(/\s+/g, "-");
  const teamsDir = join(state.currentProject, ".quorum", "teams");
  const filePath = join(teamsDir, id + ".yaml");

  if (existsSync(filePath)) {
    return c.json({ error: `Team "${id}" already exists` }, 400);
  }

  mkdirSync(teamsDir, { recursive: true });
  const yaml = `name: ${body.name.trim()}\ndefault_path: [${body.defaultPath.join(", ")}]\n`;
  writeFileSync(filePath, yaml);
  return c.json({ success: true, id });
});

app.put("/api/teams/:id", async (c) => {
  const id = c.req.param("id");
  const body = await c.req.json<{ name?: string; defaultPath?: string[] }>();
  const state = getState();
  if (!state.currentProject) return c.json({ error: "no project selected" }, 400);

  const filePath = join(state.currentProject, ".quorum", "teams", id + ".yaml");
  if (!existsSync(filePath)) return c.json({ error: "team not found" }, 404);

  let content = readFileSync(filePath, "utf-8");
  if (body.name != null) {
    content = content.replace(/^name:\s*.+/m, `name: ${body.name.trim()}`);
  }
  if (body.defaultPath != null) {
    content = content.replace(
      /^default_path:\s*.+/m,
      `default_path: [${body.defaultPath.join(", ")}]`
    );
  }
  writeFileSync(filePath, content);
  return c.json({ success: true });
});

app.delete("/api/teams/:id", async (c) => {
  const id = c.req.param("id");
  const state = getState();
  if (!state.currentProject) return c.json({ error: "no project selected" }, 400);

  const filePath = join(state.currentProject, ".quorum", "teams", id + ".yaml");
  if (!existsSync(filePath)) return c.json({ error: "team not found" }, 404);

  unlinkSync(filePath);
  return c.json({ success: true });
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

app.post("/api/agents", async (c) => {
  const body = await c.req.json<{
    role: string;
    name: string;
    description?: string;
    targetDir?: string;
    skill?: string;
  }>();
  if (!body.role || !body.name) {
    return c.json({ error: "role and name are required" }, 400);
  }
  const args = ["agent", "create", "--role", body.role, "--name", body.name, "--no-ai"];
  if (body.description) args.push("--description", body.description);
  if (body.targetDir) args.push("--target-dir", body.targetDir);
  if (body.skill) args.push("--skill", body.skill);
  const result = await execDaemon(...args);
  return c.json({
    success: result.success,
    output: result.stdout,
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
  writeFileSync(contextPath, body.content);
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
    const rows = freshQuery<{
      agent: string;
      tasks: number;
      total_cost: number;
      avg_cost: number;
    }>(
      "SELECT agent, COUNT(*) as tasks, " +
      "ROUND(COALESCE(SUM(cost), 0), 2) as total_cost, " +
      "ROUND(COALESCE(AVG(cost), 0), 2) as avg_cost " +
      "FROM tasks WHERE cost > 0 GROUP BY agent ORDER BY total_cost DESC"
    );
    return c.json(rows);
  } catch {
    return c.json([]);
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
  const body = await c.req.json<{ goal: string; team?: string }>();
  if (!body.goal) return c.json({ error: "goal is required" }, 400);

  // Record current max ID before spawning
  const before = freshQuery<{ max_id: number | null }>(
    "SELECT MAX(id) as max_id FROM conversations"
  );
  const maxIdBefore = before[0]?.max_id ?? 0;

  // Spawn daemon in background — it creates the conversation and runs the dispatch loop
  const teamTag = body.team ? ` [team: ${body.team}]` : "";
  console.log(`[converse] spawning daemon for: "${body.goal}"${teamTag}`);
  if (body.team) {
    spawnDaemon("converse", "--team", body.team, body.goal);
  } else {
    spawnDaemon("converse", body.goal);
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
