import { test, expect, beforeAll, afterAll } from "bun:test";
import { mkdtempSync, writeFileSync, chmodSync, rmSync } from "fs";
import { join } from "path";
import { tmpdir } from "os";
import { createDocentAskApp, stepsFromStderr, DOCENT_ASK_PATH } from "./docentAsk";

// A fake `ownagent.py ask` that answers quickly: two step lines on stderr,
// the answer on stdout, exit 0.
const FAKE_OK = `#!/bin/bash
echo "brain: fake-cli · project: scratch · mode: agentic" >&2
echo "  [step 1] ACTION: search('invoker')" >&2
sleep 0.1
echo "  [step 2] ANSWER" >&2
echo "invoker.h builds the claude -p command line."
exit 0
`;

const FAKE_FAIL = `#!/bin/bash
echo "  [step 1] ACTION: search('nope')" >&2
echo "brain error: no such vault" >&2
exit 3
`;

// The shape that made the old timeout unenforceable. Plain \`sleep 5\` in the
// foreground: bash itself dies on SIGTERM (no trap ⇒ default disposition), but
// the orphaned \`sleep\` inherits — and keeps open — stdout and stderr. Draining
// those pipes therefore takes the full 5 s no matter who we killed.
const FAKE_SLOW = `#!/bin/bash
echo "  [step 1] ACTION: search('slow')" >&2
sleep 5
echo "never"
`;

// The same hazard with the parent out of the picture entirely: the script
// exits 0 immediately while a backgrounded grandchild holds the inherited
// pipes for 5 s. \`await new Response(proc.stderr).text()\` hangs even though
// the process we spawned is already gone — exactly `python3 ownagent.py`
// returning while `claude -p` lingers.
const FAKE_GRANDCHILD = `#!/bin/bash
( sleep 5 ) &
echo "  [step 1] ACTION: search('slow')" >&2
echo "parent answer"
exit 0
`;

let dir: string;
const scripts: Record<string, string> = {};

beforeAll(() => {
  dir = mkdtempSync(join(tmpdir(), "docent-ask-"));
  for (const [name, src] of Object.entries({
    ok: FAKE_OK,
    fail: FAKE_FAIL,
    slow: FAKE_SLOW,
    grandchild: FAKE_GRANDCHILD,
  })) {
    const p = join(dir, `fake-${name}.sh`);
    writeFileSync(p, src);
    chmodSync(p, 0o755);
    scripts[name] = p;
  }
});

afterAll(() => {
  rmSync(dir, { recursive: true, force: true });
});

// Serve ONLY the sub-app on an ephemeral port and time the round trip. The
// question doubles as a marker: it lands in the fake's argv, so `pgrep -f`
// can answer "is that child still alive?".
async function ask(
  script: string,
  opts: { timeoutMs?: number; question?: string } = {},
): Promise<{ res: Response; body: any; elapsed: number }> {
  const app = createDocentAskApp({
    getProject: () => dir,
    docentDir: dir,
    timeoutMs: opts.timeoutMs,
    env: { DOCENT_CMD: JSON.stringify(["bash", script]) },
  });
  const server = Bun.serve({ port: 0, fetch: app.fetch });
  try {
    const t0 = Date.now();
    const res = await fetch(`http://localhost:${server.port}${DOCENT_ASK_PATH}`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ question: opts.question ?? "who builds the CLI line?" }),
    });
    const body = await res.json();
    return { res, body, elapsed: Date.now() - t0 };
  } finally {
    server.stop(true);
  }
}

/** pids whose full argv contains `marker` (never matches pgrep itself). */
async function pgrepF(marker: string): Promise<string[]> {
  const p = Bun.spawn(["pgrep", "-f", marker], { stdout: "pipe", stderr: "ignore" });
  const out = await new Response(p.stdout).text();
  await p.exited;
  return out.split("\n").filter(Boolean);
}

async function goneWithin(marker: string, ms: number): Promise<boolean> {
  const deadline = Date.now() + ms;
  for (;;) {
    if ((await pgrepF(marker)).length === 0) return true;
    if (Date.now() > deadline) return false;
    await Bun.sleep(25);
  }
}

test("a fast agent answers 200 with the answer and its step log", async () => {
  const { res, body, elapsed } = await ask(scripts.ok!);
  expect(res.status).toBe(200);
  expect(body.answer).toBe("invoker.h builds the claude -p command line.");
  expect(body.steps).toEqual(["[step 1] ACTION: search('invoker')", "[step 2] ANSWER"]);
  // the non-step preamble ("brain: …") is not mistaken for a step
  expect(elapsed).toBeLessThan(2000);
});

test("a hung agent times out fast — the orphan's pipes do not hold the response", async () => {
  const marker = `docent-ask-slow-${process.pid}-${Date.now()}`;
  const { res, body, elapsed } = await ask(scripts.slow!, { timeoutMs: 300, question: marker });

  // THE item: the fake runs 5 s; the answer must not.
  expect(elapsed).toBeLessThan(1000);
  expect(res.status).toBe(504);
  expect(body.error).toContain("timed out");

  // …and we really did signal the child, rather than just abandoning it.
  expect(await goneWithin(marker, 1000)).toBe(true);
});

test("a grandchild holding stderr after the parent exits still cannot stall the route", async () => {
  const marker = `docent-ask-gc-${process.pid}-${Date.now()}`;
  const { res, elapsed } = await ask(scripts.grandchild!, { timeoutMs: 300, question: marker });
  // The parent exits at once with an answer on stdout, but the backgrounded
  // `sleep 5` keeps both pipes open. Whether that grandchild dies is the
  // agent's own business (the python SIGTERM handler); the RESPONSE is ours.
  expect(elapsed).toBeLessThan(1000);
  expect(res.status).toBe(504);
});

test("a non-zero exit becomes 500 with the last stderr line", async () => {
  const { res, body } = await ask(scripts.fail!);
  expect(res.status).toBe(500);
  expect(body.error).toBe("brain error: no such vault");
});

test("no project selected → 400, and nothing is spawned", async () => {
  const app = createDocentAskApp({ getProject: () => null, docentDir: dir });
  const res = await app.request(DOCENT_ASK_PATH, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ question: "hi" }),
  });
  expect(res.status).toBe(400);
  expect((await res.json()).error).toContain("No project");
});

test("empty question → 400", async () => {
  const app = createDocentAskApp({ getProject: () => dir, docentDir: dir });
  const res = await app.request(DOCENT_ASK_PATH, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ question: "   " }),
  });
  expect(res.status).toBe(400);
  expect((await res.json()).error).toContain("question is required");
});

test("stepsFromStderr keeps step lines only, trimmed", () => {
  expect(
    stepsFromStderr("brain: fake · mode: agentic\n  [step 1] ACTION: search('x')\n\n  [step 2] ANSWER\n"),
  ).toEqual(["[step 1] ACTION: search('x')", "[step 2] ANSWER"]);
  expect(stepsFromStderr("")).toEqual([]);
});
