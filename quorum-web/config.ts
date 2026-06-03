import { resolve, join } from "path";
import { readFileSync, writeFileSync } from "fs";

// Resolve paths relative to quorum repo root (one level up from quorum-web/)
export const repoRoot = resolve(import.meta.dir, "..");

// Legacy hardcoded config — kept for backward compatibility
export const config = {
  port: 3100,
  daemonBin: resolve(repoRoot, "build/quorum_daemon"),
  configPath: resolve(repoRoot, "configs/hello-world.yaml"),
  dbPath: resolve(repoRoot, "data/quorum.db"),
};

// --- Dynamic project selection ---

export interface WebState {
  currentProject: string | null;
  recentProjects: string[];
}

export interface ProjectConfig {
  projectPath: string;
  configPath: string;
  dbPath: string;
  agentsDir: string;
  vaultsDir: string;
  daemonBin: string;
}

const STATE_FILE = join(import.meta.dir, ".quorum-web-state.json");

export function getState(): WebState {
  try {
    const raw = readFileSync(STATE_FILE, "utf-8");
    const parsed = JSON.parse(raw);
    return {
      currentProject: parsed.currentProject ?? null,
      recentProjects: Array.isArray(parsed.recentProjects) ? parsed.recentProjects : [],
    };
  } catch {
    return { currentProject: null, recentProjects: [] };
  }
}

export function saveState(state: WebState): void {
  writeFileSync(STATE_FILE, JSON.stringify(state, null, 2), "utf-8");
}

export function setCurrentProject(path: string): void {
  const state = getState();
  state.currentProject = path;
  // Prepend to recentProjects, dedup, cap at 10
  state.recentProjects = [path, ...state.recentProjects.filter((p) => p !== path)].slice(0, 10);
  saveState(state);
}

export function getProjectConfig(projectPath: string): ProjectConfig {
  const quorumDir = join(projectPath, ".quorum");
  return {
    projectPath,
    configPath: join(quorumDir, "config.yaml"),
    dbPath: join(quorumDir, "quorum.db"),
    agentsDir: join(quorumDir, "agents"),
    vaultsDir: join(quorumDir, "vaults"),
    daemonBin: resolve(repoRoot, "build/quorum_daemon"),
  };
}
