import { readFileSync, unlinkSync, existsSync } from "fs";
import { join } from "path";
import { config, repoRoot, getState, getProjectConfig } from "../config";

// Resolve dynamic project config, falling back to legacy hardcoded paths
function getCurrentProjectConfig(): { daemonBin: string; configPath: string; projectPath: string } {
  const state = getState();
  if (state.currentProject) {
    const pc = getProjectConfig(state.currentProject);
    return { daemonBin: pc.daemonBin, configPath: pc.configPath, projectPath: pc.projectPath };
  }
  return { daemonBin: config.daemonBin, configPath: config.configPath, projectPath: repoRoot };
}

// Read pid_file path from the YAML config (simple grep — avoids a YAML parser dep)
function getPidFilePath(): string | null {
  try {
    const pc = getCurrentProjectConfig();
    const yaml = readFileSync(pc.configPath, "utf-8");
    const match = yaml.match(/pid_file:\s*(.+)/);
    if (!match) return null;
    const pidFileRel = match[1].trim();
    // Absolute paths used as-is; relative paths resolved against project root
    if (pidFileRel.startsWith("/")) return pidFileRel;
    return join(pc.projectPath, pidFileRel);
  } catch {
    return null;
  }
}

// Reap a genuinely stale PID lock on web server startup. A LIVE daemon is
// never touched — it may be mid-conversation, and restarting the web server
// must not kill in-flight work (learned the hard way 2026-07-11: the old
// kill-if-alive behavior murdered a running brainstorm on `make web`).
export function cleanupStaleDaemon(): void {
  const pidFile = getPidFilePath();
  if (!pidFile || !existsSync(pidFile)) return;

  try {
    const pid = parseInt(readFileSync(pidFile, "utf-8").trim(), 10);
    if (isNaN(pid)) {
      unlinkSync(pidFile);
      return;
    }
    try {
      process.kill(pid, 0);
      // Process alive — leave it alone.
      console.log(`[cleanup] daemon PID ${pid} is alive — leaving it running`);
    } catch {
      // Process dead — remove the stale PID file.
      console.log(`[cleanup] removing stale PID file (PID ${pid} dead)`);
      unlinkSync(pidFile);
    }
  } catch {}
}

// Check if daemon process is alive (non-destructive — signal 0)
export function isDaemonRunning(): boolean {
  const pidFile = getPidFilePath();
  if (!pidFile || !existsSync(pidFile)) return false;
  try {
    const pid = parseInt(readFileSync(pidFile, "utf-8").trim(), 10);
    if (isNaN(pid)) return false;
    process.kill(pid, 0); // throws if dead
    return true;
  } catch {
    return false;
  }
}

export interface DaemonResult {
  success: boolean;
  stdout: string;
  stderr: string;
  exitCode: number;
}

const daemonEnv = {
  ...process.env,
  // Remove CLAUDECODE to avoid nesting detection if web server runs inside Claude Code
  CLAUDECODE: undefined,
};

// For short-lived commands (gate, close, resume, status)
export async function execDaemon(...args: string[]): Promise<DaemonResult> {
  const pc = getCurrentProjectConfig();
  const proc = Bun.spawn([pc.daemonBin, "--config", pc.configPath, ...args], {
    cwd: pc.projectPath,
    stdout: "pipe",
    stderr: "pipe",
    env: daemonEnv,
  });

  const stdout = await new Response(proc.stdout).text();
  const stderr = await new Response(proc.stderr).text();
  const exitCode = await proc.exited;

  return {
    success: exitCode === 0,
    stdout: stdout.trim(),
    stderr: stderr.trim(),
    exitCode,
  };
}

// For commands that run without --config, using an explicit cwd (e.g. init)
export async function execDaemonAt(cwd: string, ...args: string[]): Promise<DaemonResult> {
  const proc = Bun.spawn([config.daemonBin, ...args], {
    cwd,
    stdout: "pipe",
    stderr: "pipe",
    env: daemonEnv,
  });

  const stdout = await new Response(proc.stdout).text();
  const stderr = await new Response(proc.stderr).text();
  const exitCode = await proc.exited;

  return {
    success: exitCode === 0,
    stdout: stdout.trim(),
    stderr: stderr.trim(),
    exitCode,
  };
}

// For the long, read-only `quorum ask` call (Phase 14 T6 — recap web button).
//
// `ask` is a self-contained `claude -p` invocation that can run for MINUTES; it
// does NOT use --config (it resolves the project via --project <path>), so we
// deliberately DON'T pass --config here — unlike execDaemon. The binary on PATH
// (`quorum`) is a symlink to this same daemonBin, so `<daemonBin> ask ...` ==
// `quorum ask ...`.
//
// `ask` prints the synthesized answer to STDOUT and progress ("Asking the
// recap...") to STDERR; on resolution/agent errors it prints to STDERR and
// exits non-zero. We await with a generous timeout (recap is a multi-minute
// claude call) and surface stderr to the caller so the popup can show the real
// error (e.g. recap knower not set up) instead of failing silently.
const ASK_TIMEOUT_MS = 8 * 60 * 1000; // 8 min — recap is a multi-minute claude -p call

export async function execAsk(
  projectPath: string,
  agent: string,
  prompt: string,
): Promise<DaemonResult> {
  const proc = Bun.spawn(
    [config.daemonBin, "ask", "--agent", agent, "--project", projectPath, prompt],
    {
      cwd: projectPath,
      stdout: "pipe",
      stderr: "pipe",
      env: daemonEnv,
    },
  );

  let timedOut = false;
  const timer = setTimeout(() => {
    timedOut = true;
    try { proc.kill(); } catch {}
  }, ASK_TIMEOUT_MS);

  const stdout = await new Response(proc.stdout).text();
  const stderr = await new Response(proc.stderr).text();
  const exitCode = await proc.exited;
  clearTimeout(timer);

  if (timedOut) {
    return {
      success: false,
      stdout: stdout.trim(),
      stderr: `recap timed out after ${ASK_TIMEOUT_MS / 60000} min`,
      exitCode: exitCode || 124,
    };
  }

  return {
    success: exitCode === 0,
    stdout: stdout.trim(),
    stderr: stderr.trim(),
    exitCode,
  };
}

// For long-running commands (converse) — spawns detached, doesn't wait
export function spawnDaemon(...args: string[]): void {
  const pc = getCurrentProjectConfig();
  const proc = Bun.spawn([pc.daemonBin, "--config", pc.configPath, ...args], {
    cwd: pc.projectPath,
    stdout: "inherit",
    stderr: "inherit",
    env: daemonEnv,
  });
  // Unref so the web server can exit even if daemon is still running
  proc.unref();
}
