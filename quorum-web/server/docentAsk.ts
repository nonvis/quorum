// POST /api/docent/ask — the buffered Docent answer.
//
// The SSE sibling (docentStream.ts) is the default UI path; this one stays as
// the fallback for callers that just want {answer, steps} in one shot.
//
// Why it is a module: the inline route it replaces had an UNENFORCEABLE
// timeout. It killed the child on the timer and then did
//
//     const stderr = await new Response(proc.stderr).text();
//
// unconditionally. `python3 ownagent.py` hands its stderr straight to the
// `claude -p` grandchild, which keeps that pipe open after python is dead — so
// the await never returned and DOCENT_TIMEOUT_MS never fired. The fix is not a
// bigger kill; it is refusing to let the RESPONSE depend on a pipe a process we
// do not control still holds.
//
// Banking is unaffected: `ownagent.py ask` banks the transcript itself
// (cmd_ask → bank.bank_record, before it prints the answer). The web never
// writes the bank — not here, and not on the timeout path, where we simply
// stop reading.
//
// This module deliberately imports nothing from ./index.ts — that module's
// load reads web state and reaps/spawns daemons. Everything it needs arrives
// as deps, so a test can serve it standalone.

import { Hono } from "hono";
import { resolveDocentCmd, isStepLine, fmtDuration } from "./docentStream";

export interface DocentAskDeps {
  /** the currently selected project root, or null */
  getProject: () => string | null;
  /** cwd for the agent (the quorum-own-agent/ dir) */
  docentDir: string;
  /** hard kill after this long */
  timeoutMs?: number;
  /** defaults to process.env — the DOCENT_CMD test seam is read from here */
  env?: Record<string, string | undefined>;
}

export const DOCENT_ASK_PATH = "/api/docent/ask";
const DEFAULT_TIMEOUT_MS = 4 * 60 * 1000;

/**
 * After SIGTERM, how long we let the child flush and die before answering
 * anyway. Bounded on purpose: a grandchild holding the pipe must not be able
 * to extend the response by even one second.
 */
const TIMEOUT_GRACE_MS = 300;

const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));

/**
 * Drain a piped stream to a string, with an escape hatch.
 *
 * `new Response(stream).text()` cannot be abandoned: it locks the stream and
 * only settles at EOF, which is precisely the hang. Here the reader stays in
 * reach so the timeout path can `release()` its end of the pipe and walk away.
 */
export function readAll(stream: ReadableStream<Uint8Array>): {
  text: Promise<string>;
  release: () => void;
} {
  const reader = stream.getReader();
  const dec = new TextDecoder();
  let out = "";
  const text = (async () => {
    try {
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        out += dec.decode(value, { stream: true });
      }
    } catch {
      // cancelled, or the pipe broke — return what we have
    }
    return out;
  })();
  return {
    text,
    release: () => {
      try {
        void reader.cancel().catch(() => {});
      } catch {}
    },
  };
}

/** The step log the agent writes to stderr, one trimmed line per step. */
export function stepsFromStderr(stderr: string): string[] {
  return stderr
    .split("\n")
    .map((l) => l.trim())
    .filter((l) => l && isStepLine(l));
}

export function createDocentAskApp(deps: DocentAskDeps): Hono {
  const app = new Hono();
  const timeoutMs = deps.timeoutMs ?? DEFAULT_TIMEOUT_MS;

  app.post(DOCENT_ASK_PATH, async (c) => {
    const project = deps.getProject();
    if (!project) return c.json({ error: "No project selected" }, 400);
    const body = await c.req
      .json<{ question?: string; singleShot?: boolean }>()
      .catch(() => ({}) as { question?: string; singleShot?: boolean });
    const question = body.question?.trim();
    if (!question) return c.json({ error: "question is required" }, 400);

    const env = deps.env ?? (process.env as Record<string, string | undefined>);
    // no --quiet: stdout = answer only, stderr = the step log we surface below
    const argv = [...resolveDocentCmd(env.DOCENT_CMD), "ask", "--project", project];
    if (body.singleShot) argv.push("--single-shot");
    argv.push(question);

    const proc = Bun.spawn(argv, { cwd: deps.docentDir, stdout: "pipe", stderr: "pipe" });

    const out = readAll(proc.stdout as ReadableStream<Uint8Array>);
    const err = readAll(proc.stderr as ReadableStream<Uint8Array>);

    const completed = (async () => {
      const [stdout, stderr] = await Promise.all([out.text, err.text]);
      return { stdout, stderr, exitCode: await proc.exited };
    })();
    // The race below may abandon this promise; make sure nothing it throws
    // surfaces as an unhandled rejection.
    completed.catch(() => {});

    const TIMED_OUT = Symbol("timeout");
    let timer: ReturnType<typeof setTimeout> | undefined;
    const deadline = new Promise<typeof TIMED_OUT>((r) => {
      timer = setTimeout(() => r(TIMED_OUT), timeoutMs);
    });

    const raced = await Promise.race([completed, deadline]);

    if (raced === TIMED_OUT) {
      // SIGTERM, not SIGKILL: the agent's own handler is what reaches the
      // `claude -p` grandchild. Then give it a bounded moment and answer
      // regardless — the pipes are explicitly NOT awaited past the grace.
      try {
        proc.kill("SIGTERM");
      } catch {}
      await Promise.race([completed, sleep(TIMEOUT_GRACE_MS)]);
      out.release();
      err.release();
      return c.json({ error: `docent timed out after ${fmtDuration(timeoutMs)}` }, 504);
    }

    clearTimeout(timer);
    const { stdout, stderr, exitCode } = raced;

    if (exitCode !== 0) {
      return c.json({ error: stderr.trim().split("\n").pop() || "docent failed" }, 500);
    }
    // stderr carries the step log ("[step 1] ACTION: search(…)") — surface it
    // so the panel can show how the answer was reached.
    return c.json({ answer: stdout.trim(), steps: stepsFromStderr(stderr) });
  });

  return app;
}
