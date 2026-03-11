import { readFileSync, unlinkSync, existsSync } from "fs";
import { config, repoRoot } from "../config";

// Read pid_file path from the YAML config (simple grep — avoids a YAML parser dep)
function getPidFilePath(): string | null {
  try {
    const yaml = readFileSync(config.configPath, "utf-8");
    const match = yaml.match(/pid_file:\s*(.+)/);
    return match ? match[1].trim() : null;
  } catch {
    return null;
  }
}

// Kill stale daemon and remove PID lock on web server startup
export function cleanupStaleDaemon(): void {
  const pidFile = getPidFilePath();
  if (!pidFile || !existsSync(pidFile)) return;

  try {
    const pid = parseInt(readFileSync(pidFile, "utf-8").trim(), 10);
    if (isNaN(pid)) {
      unlinkSync(pidFile);
      return;
    }
    // Check if process is alive
    try {
      process.kill(pid, 0);
      // Process alive — kill it
      console.log(`[cleanup] killing stale daemon PID ${pid}`);
      process.kill(pid, "SIGTERM");
      // Give it a moment, then remove PID file
      setTimeout(() => {
        try { unlinkSync(pidFile); } catch {}
      }, 500);
    } catch {
      // Process dead — just remove stale PID file
      console.log(`[cleanup] removing stale PID file (PID ${pid} dead)`);
      unlinkSync(pidFile);
    }
  } catch {}
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
  const proc = Bun.spawn([config.daemonBin, "--config", config.configPath, ...args], {
    cwd: repoRoot,
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

// For long-running commands (converse) — spawns detached, doesn't wait
export function spawnDaemon(...args: string[]): void {
  const proc = Bun.spawn([config.daemonBin, "--config", config.configPath, ...args], {
    cwd: repoRoot,
    stdout: "inherit",
    stderr: "inherit",
    env: daemonEnv,
  });
  // Unref so the web server can exit even if daemon is still running
  proc.unref();
}
