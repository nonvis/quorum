import { resolve } from "path";

// Resolve paths relative to quorum repo root (one level up from quorum-web/)
export const repoRoot = resolve(import.meta.dir, "..");

export const config = {
  port: 3100,
  daemonBin: resolve(repoRoot, "build/quorum_daemon"),
  configPath: resolve(repoRoot, "configs/hello-world.yaml"),
  dbPath: resolve(repoRoot, "data/quorum.db"),
};
