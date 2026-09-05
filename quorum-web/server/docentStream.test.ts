import { test, expect, beforeAll, afterAll } from "bun:test";
import { mkdtempSync, writeFileSync, chmodSync, rmSync } from "fs";
import { join } from "path";
import { tmpdir } from "os";
import { createDocentStreamApp, resolveDocentCmd, isStepLine, DOCENT_STREAM_PATH } from "./docentStream";

// A fake `ownagent.py ask` — three step lines on stderr 0.3 s apart, then the
// answer on stdout. Total ≥ 0.9 s, so "the first step arrived early" and "the
// run really did take that long" are separable claims.
const FAKE_OK = `#!/bin/bash
echo "brain: fake-cli · project: scratch · mode: agentic" >&2
echo "  [step 1] ACTION: search('invoker')" >&2
sleep 0.3
echo "  [step 2] ACTION: read('quorum-core/src/agent/invoker.h')" >&2
sleep 0.3
echo "  [step 3] ANSWER" >&2
sleep 0.3
echo "invoker.h builds the claude -p command line."
exit 0
`;

const FAKE_FAIL = `#!/bin/bash
echo "  [step 1] ACTION: search('nope')" >&2
echo "brain error: no such vault" >&2
exit 3
`;

// The orphan-child case: killing bash does NOT kill `sleep`, which keeps the
// inherited stderr pipe open — exactly what `python3 ownagent.py` does with
// its `claude -p` child. A timeout that waits for the pipe never fires.
const FAKE_HANG = `#!/bin/bash
echo "  [step 1] ACTION: search('slow')" >&2
sleep 2
echo "never"
`;

let dir: string;
const scripts: Record<string, string> = {};

beforeAll(() => {
  dir = mkdtempSync(join(tmpdir(), "docent-stream-"));
  for (const [name, src] of Object.entries({ ok: FAKE_OK, fail: FAKE_FAIL, hang: FAKE_HANG })) {
    const p = join(dir, `fake-${name}.sh`);
    writeFileSync(p, src);
    chmodSync(p, 0o755);
    scripts[name] = p;
  }
});

afterAll(() => {
  rmSync(dir, { recursive: true, force: true });
});

interface SseEvent {
  event: string;
  data: string;
  /** ms since the request went out */
  at: number;
}

// Serve ONLY the sub-app on an ephemeral port, POST, and timestamp each SSE
// event as its frame completes.
async function askStream(
  script: string,
  opts: { timeoutMs?: number; body?: unknown } = {},
): Promise<{ events: SseEvent[]; total: number }> {
  const app = createDocentStreamApp({
    getProject: () => dir,
    docentDir: dir,
    timeoutMs: opts.timeoutMs,
    env: { DOCENT_CMD: JSON.stringify(["bash", script]) },
  });
  const server = Bun.serve({ port: 0, fetch: app.fetch });
  try {
    const t0 = Date.now();
    const res = await fetch(`http://localhost:${server.port}${DOCENT_STREAM_PATH}`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(opts.body ?? { question: "who builds the CLI line?" }),
    });
    expect(res.headers.get("content-type")).toContain("text/event-stream");

    const events: SseEvent[] = [];
    const reader = res.body!.getReader();
    const dec = new TextDecoder();
    let buf = "";
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      buf += dec.decode(value, { stream: true });
      for (let i = buf.indexOf("\n\n"); i >= 0; i = buf.indexOf("\n\n")) {
        const frame = buf.slice(0, i);
        buf = buf.slice(i + 2);
        const ev = /^event: (.*)$/m.exec(frame)?.[1] ?? "";
        const data = /^data: ([\s\S]*)$/m.exec(frame)?.[1] ?? "";
        events.push({ event: ev, data, at: Date.now() - t0 });
      }
    }
    return { events, total: Date.now() - t0 };
  } finally {
    server.stop(true);
  }
}

test("steps stream while the agent is still running", async () => {
  const { events, total } = await askStream(scripts.ok!);
  const kinds = events.map((e) => e.event);

  // shape: three steps, then the answer, then done
  expect(kinds).toEqual(["step", "step", "step", "answer", "done"]);

  // the run really did take ≥0.9 s — otherwise "arrived early" proves nothing
  expect(total).toBeGreaterThanOrEqual(900);

  // …and the first step was on screen long before the last sleep finished
  const firstStep = events[0]!;
  expect(firstStep.at).toBeLessThan(600);
  expect(firstStep.data).toContain("[step 1] ACTION: search('invoker')");

  // each later step also beats the end of the run (not one flush at exit)
  expect(events[1]!.at).toBeLessThan(900);

  // the non-step preamble ("brain: …") is not mistaken for a step
  for (const e of events.filter((x) => x.event === "step")) {
    expect(e.data).toContain("[step");
  }

  const answer = JSON.parse(events[3]!.data);
  expect(answer.answer).toBe("invoker.h builds the claude -p command line.");
  expect(answer.steps).toHaveLength(3);
  expect(JSON.parse(events[4]!.data)).toEqual({ exitCode: 0 });
});

test("a non-zero exit becomes an error event, then done with the code", async () => {
  const { events } = await askStream(scripts.fail!);
  expect(events.map((e) => e.event)).toEqual(["step", "error", "done"]);
  expect(JSON.parse(events[1]!.data).error).toBe("brain error: no such vault");
  expect(JSON.parse(events[2]!.data)).toEqual({ exitCode: 3 });
});

test("a hung agent times out without waiting for its orphaned child's pipes", async () => {
  const { events, total } = await askStream(scripts.hang!, { timeoutMs: 400 });
  expect(total).toBeLessThan(1500); // the stream ends at the timeout, not at sleep 2
  expect(events.map((e) => e.event)).toEqual(["step", "error", "done"]);
  expect(JSON.parse(events[1]!.data).error).toContain("timed out");
});

test("no project selected → 400, and nothing is spawned", async () => {
  const app = createDocentStreamApp({ getProject: () => null, docentDir: dir });
  const res = await app.request(DOCENT_STREAM_PATH, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ question: "hi" }),
  });
  expect(res.status).toBe(400);
  expect((await res.json()).error).toContain("No project");
});

test("empty question → 400", async () => {
  const app = createDocentStreamApp({ getProject: () => dir, docentDir: dir });
  const res = await app.request(DOCENT_STREAM_PATH, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ question: "   " }),
  });
  expect(res.status).toBe(400);
  expect((await res.json()).error).toContain("question is required");
});

test("DOCENT_CMD seam: JSON argv, whitespace form, and the real default", () => {
  expect(resolveDocentCmd(undefined)).toEqual(["python3", "ownagent.py"]);
  expect(resolveDocentCmd("  ")).toEqual(["python3", "ownagent.py"]);
  expect(resolveDocentCmd('["bash","/tmp/f.sh"]')).toEqual(["bash", "/tmp/f.sh"]);
  expect(resolveDocentCmd("bash /tmp/f.sh")).toEqual(["bash", "/tmp/f.sh"]);
  // malformed JSON falls back to the whitespace split rather than throwing
  expect(resolveDocentCmd('["bash",')).toEqual(['["bash",']);
});

test("isStepLine matches the agent's step lines only", () => {
  expect(isStepLine("  [step 1] ACTION: search('x')")).toBe(true);
  expect(isStepLine("  [step 2] ANSWER")).toBe(true);
  expect(isStepLine("  [step 3] malformed reply — nudging")).toBe(true);
  expect(isStepLine("brain: claude-cli · project: quorum · mode: agentic")).toBe(false);
  expect(isStepLine("(bank write failed: disk full)")).toBe(false);
});
