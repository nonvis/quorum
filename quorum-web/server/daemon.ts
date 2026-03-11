import { config } from "../config";

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
    stdout: "ignore",
    stderr: "ignore",
    env: daemonEnv,
  });
  // Unref so the web server can exit even if daemon is still running
  proc.unref();
}
