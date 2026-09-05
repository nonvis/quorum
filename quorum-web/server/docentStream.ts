// POST /api/docent/ask/stream — the Docent step log as Server-Sent Events.
//
// The buffered sibling (POST /api/docent/ask in index.ts) waits for the
// process to exit and only then hands back {answer, steps}: the panel shows
// nothing for the whole agentic run (~60 s). Here every `  [step N] …` line
// ownagent.py writes to stderr (loop.py:86-109) is forwarded the moment it
// arrives.
//
// Banking is unaffected: `ownagent.py ask` banks the transcript itself
// (cmd_ask → bank.bank_record, before it prints the answer), so a streamed run
// banks exactly like a buffered one. The web never writes the bank.
//
// This module deliberately imports nothing from ./index.ts — that module's
// load reads web state and reaps/spawns daemons. Everything it needs arrives
// as deps, so a test can serve it standalone.

import { Hono } from "hono";

export interface DocentStreamDeps {
  /** the currently selected project root, or null */
  getProject: () => string | null;
  /** cwd for the agent (the quorum-own-agent/ dir) */
  docentDir: string;
  /** hard kill after this long */
  timeoutMs?: number;
  /** defaults to process.env — the DOCENT_CMD test seam is read from here */
  env?: Record<string, string | undefined>;
}

export const DOCENT_STREAM_PATH = "/api/docent/ask/stream";
const DEFAULT_TIMEOUT_MS = 4 * 60 * 1000;

/**
 * The command that runs the agent, minus the `ask …` arguments.
 *
 * TEST SEAM — `DOCENT_CMD` replaces the `python3 ownagent.py` prefix:
 *   - a JSON array  → used as argv verbatim, e.g. `["bash","/tmp/fake.sh"]`
 *   - anything else → split on whitespace, e.g. `bash /tmp/fake.sh`
 * The ask arguments are appended to whatever it yields.
 */
export function resolveDocentCmd(raw: string | undefined): string[] {
  const s = (raw ?? "").trim();
  if (!s) return ["python3", "ownagent.py"];
  if (s.startsWith("[")) {
    try {
      const parsed = JSON.parse(s);
      if (Array.isArray(parsed) && parsed.length > 0) return parsed.map(String);
    } catch {
      // fall through to the whitespace split
    }
  }
  return s.split(/\s+/);
}

/** Is this stderr line one of the agent's per-step log lines? */
export function isStepLine(line: string): boolean {
  return line.includes("[step");
}

function fmtDuration(ms: number): string {
  return ms >= 60_000 ? `${Math.round(ms / 60_000)} min` : `${(ms / 1000).toFixed(1)} s`;
}

/**
 * Read a piped stream line-by-line AS IT ARRIVES.
 *
 * This is the whole item: reading the stream to completion first (what the
 * buffered endpoint does) means every step lands at exit.
 */
export async function pumpLines(
  stream: ReadableStream<Uint8Array>,
  onLine: (line: string) => void,
): Promise<void> {
  const reader = stream.getReader();
  const dec = new TextDecoder();
  let buf = "";
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    buf += dec.decode(value, { stream: true });
    for (let i = buf.indexOf("\n"); i >= 0; i = buf.indexOf("\n")) {
      onLine(buf.slice(0, i));
      buf = buf.slice(i + 1);
    }
  }
  if (buf.trim()) onLine(buf);
}

export function createDocentStreamApp(deps: DocentStreamDeps): Hono {
  const app = new Hono();
  const timeoutMs = deps.timeoutMs ?? DEFAULT_TIMEOUT_MS;

  app.post(DOCENT_STREAM_PATH, async (c) => {
    const project = deps.getProject();
    if (!project) return c.json({ error: "No project selected" }, 400);
    const body = await c.req
      .json<{ question?: string; singleShot?: boolean }>()
      .catch(() => ({}) as { question?: string; singleShot?: boolean });
    const question = body.question?.trim();
    if (!question) return c.json({ error: "question is required" }, 400);

    const env = deps.env ?? (process.env as Record<string, string | undefined>);
    const argv = [...resolveDocentCmd(env.DOCENT_CMD), "ask", "--project", project];
    if (body.singleShot) argv.push("--single-shot");
    argv.push(question);

    const enc = new TextEncoder();
    const stream = new ReadableStream<Uint8Array>({
      start(controller) {
        let closed = false;
        const send = (event: string, data: string) => {
          if (closed) return;
          try {
            controller.enqueue(enc.encode(`event: ${event}\ndata: ${data}\n\n`));
          } catch {
            closed = true; // client hung up
          }
        };

        // One terminal frame pair (outcome + done), whoever gets there first:
        // the agent finishing, or the timeout.
        let finished = false;
        const finish = (event: string, data: string, exitCode: number | null) => {
          if (finished) return;
          finished = true;
          send(event, data);
          send("done", JSON.stringify({ exitCode }));
          if (!closed) {
            closed = true;
            try {
              controller.close();
            } catch {}
          }
        };

        const run = async () => {
          const proc = Bun.spawn(argv, {
            cwd: deps.docentDir,
            stdout: "pipe",
            stderr: "pipe",
          });
          const timer = setTimeout(() => {
            try {
              proc.kill();
            } catch {}
            // Close the stream WITHOUT waiting for the pipes to drain: the
            // agent's own child (`claude -p`) inherits them and can hold them
            // open long after python is dead, so a drain-then-report timeout
            // is no timeout at all.
            finish("error", JSON.stringify({ error: `docent timed out after ${fmtDuration(timeoutMs)}` }), null);
          }, timeoutMs);

          // stdout is the answer — collect it while stderr streams.
          const stdoutText = new Response(proc.stdout).text();

          const steps: string[] = [];
          let lastErrLine = "";
          await pumpLines(proc.stderr as ReadableStream<Uint8Array>, (raw) => {
            const line = raw.trim();
            if (!line) return;
            lastErrLine = line;
            if (!isStepLine(line)) return;
            steps.push(line);
            send("step", line);
          });

          const answer = (await stdoutText).trim();
          const exitCode = await proc.exited;
          clearTimeout(timer);

          if (exitCode !== 0) {
            finish("error", JSON.stringify({ error: lastErrLine || "docent failed" }), exitCode);
          } else {
            finish("answer", JSON.stringify({ answer, steps }), exitCode);
          }
        };

        // Not awaited: start() must return so the response headers go out and
        // the first step can reach the client while the agent is still running.
        void run();
      },
    });

    return new Response(stream, {
      headers: {
        "Content-Type": "text/event-stream",
        "Cache-Control": "no-cache",
        Connection: "keep-alive",
        // nginx/proxy buffering would re-create the very silence this fixes
        "X-Accel-Buffering": "no",
      },
    });
  });

  return app;
}
