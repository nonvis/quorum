import { Hono } from "hono";
import { cors } from "hono/cors";
import { config, repoRoot } from "../config";
import { readFileSync } from "fs";
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
import { createSSEStream, markAutoApprove } from "./sse";

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
  // Parse target_dir and pipeline from YAML config
  try {
    const yaml = readFileSync(config.configPath, "utf-8");
    const targetDir = yaml.match(/^\s+target_dir:\s*(.+)/m)?.[1]?.trim() ?? null;
    const pipeline = yaml.match(/^\s+pipeline:\s*(.+)/m)?.[1]?.trim() ?? null;
    return c.json({ target_dir: targetDir, pipeline, config_path: config.configPath });
  } catch {
    return c.json({ target_dir: null, pipeline: null, config_path: config.configPath });
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
  const body = await c.req.json<{ goal: string; autoApprove?: boolean }>();
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

  if (conversationId && body.autoApprove) {
    markAutoApprove(conversationId);
  }

  return c.json({
    success: true,
    conversationId,
  });
});

app.post("/api/gate/:id/approve", async (c) => {
  const id = c.req.param("id");

  // If proposal text provided, update the thinker's task result before approving
  const body = await c.req.json<{ proposal?: string }>().catch(() => ({}));
  if (body.proposal) {
    dbWrite(
      "UPDATE tasks SET result = ? WHERE id = (SELECT id FROM tasks WHERE conversation_id = ? AND task_type = 'think' AND status = 'done' ORDER BY id DESC LIMIT 1)",
      [body.proposal, Number(id)]
    );
    console.log(`[gate] updated proposal for conversation ${id}`);
  }

  const result = await execDaemon("gate", "--approve", "--conversation", id);
  return c.json({
    success: result.success,
    output: result.stdout,
    error: result.stderr || undefined,
  });
});

app.post("/api/gate/:id/reject", async (c) => {
  const id = c.req.param("id");
  const result = await execDaemon("gate", "--reject", "--conversation", id);
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
