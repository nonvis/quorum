import { Hono } from "hono";
import { cors } from "hono/cors";
import { config, repoRoot } from "../config";
import { readFileSync, writeFileSync } from "fs";
import {
  getConversations,
  getConversation,
  getTasksForConversation,
  getStats,
  freshQuery,
  dbWrite,
  type Conversation,
} from "./db";
import { execDaemon, spawnDaemon, cleanupStaleDaemon } from "./daemon";
import { createSSEStream } from "./sse";

const app = new Hono();

// CORS for local dev (Vite runs on different port)
app.use("*", cors());

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

app.get("/api/config", (c) => {
  try {
    const yaml = readFileSync(config.configPath, "utf-8");

    // Parse daemon section
    const targetDir = yaml.match(/^\s+target_dir:\s*(.+)/m)?.[1]?.trim() ?? null;
    const pidFile = yaml.match(/^\s+pid_file:\s*(.+)/m)?.[1]?.trim() ?? null;
    const dataDir = yaml.match(/^\s+data_dir:\s*(.+)/m)?.[1]?.trim() ?? null;
    const logLevel = yaml.match(/^\s+log_level:\s*(.+)/m)?.[1]?.trim() ?? null;

    // Parse budget section
    const hourlyBudget = parseFloat(yaml.match(/^\s+hourly_limit_usd:\s*(.+)/m)?.[1] ?? "0") || null;

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
      budget: { hourly_limit_usd: hourlyBudget },
      conversations: { leader, default_path: defaultPath, default_budget_usd: convBudget, default_max_turns: maxRounds },
      agents: agentConfigs,
    });
  } catch {
    return c.json({
      config_path: config.configPath,
      daemon: { target_dir: null, pid_file: null, data_dir: null, log_level: null },
      budget: { hourly_limit_usd: null },
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
      hourly_limit_usd: "hourly_limit_usd",
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
  const body = await c.req.json<{ goal: string }>();
  if (!body.goal) return c.json({ error: "goal is required" }, 400);

  // Record current max ID before spawning
  const before = freshQuery<{ max_id: number | null }>(
    "SELECT MAX(id) as max_id FROM conversations"
  );
  const maxIdBefore = before[0]?.max_id ?? 0;

  // Spawn daemon in background — it creates the conversation and runs the dispatch loop
  console.log(`[converse] spawning daemon for: "${body.goal}"`);
  spawnDaemon("converse", body.goal);

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

export default {
  port: config.port,
  fetch: app.fetch,
  idleTimeout: 120, // seconds — prevent premature SSE disconnect
};
