import { Hono } from "hono";
import { cors } from "hono/cors";
import { config } from "../config";
import {
  getConversations,
  getConversation,
  getTasksForConversation,
  getStats,
} from "./db";
import { execDaemon, spawnDaemon } from "./daemon";
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

  // Spawn daemon in background — it creates the conversation and runs the dispatch loop
  spawnDaemon("converse", body.goal);

  // Give the daemon a moment to create the conversation in SQLite
  await new Promise((r) => setTimeout(r, 500));

  // Find the newly created conversation by querying DB
  const conversations = getConversations();
  const newest = conversations[0]; // ordered by id DESC
  let conversationId: number | null = null;

  if (newest && newest.goal === body.goal) {
    conversationId = newest.id;
    if (body.autoApprove && conversationId) {
      markAutoApprove(conversationId);
    }
  }

  return c.json({
    success: true,
    conversationId,
  });
});

app.post("/api/gate/:id/approve", async (c) => {
  const id = c.req.param("id");
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
  const result = await execDaemon("resume", "--conversation", id);
  return c.json({
    success: result.success,
    output: result.stdout,
    error: result.stderr || undefined,
  });
});

// -- Start --

console.log(`Quorum Web API — http://localhost:${config.port}`);
console.log(`  DB: ${config.dbPath}`);
console.log(`  Daemon: ${config.daemonBin}`);
console.log(`  Config: ${config.configPath}`);

export default {
  port: config.port,
  fetch: app.fetch,
};
