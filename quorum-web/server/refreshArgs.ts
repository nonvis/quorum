// Pure argv builder for POST /api/knower/refresh.
//
// Mirrors the daemon's own contract in quorum-core/src/cli/knower_refresh.h:
//   :74-77  the four lenses, in order
//   - --all and --knower are mutually exclusive
//   - an unknown lens name is rejected BEFORE any project work
// Mirroring it here means the web returns 400 instead of spawning a daemon
// that will exit 1 into a detached stderr nobody reads. There is no parallel
// option: a project runs one daemon, so lenses refresh in turn (Decision #87).
//
// Returns only the argv TAIL — the endpoint prepends
// ["knower", "refresh", "--project", <root>].

/** The four knower lenses, in the daemon's refresh order. */
export const KNOWER_NAMES = ["cartographer", "architect", "historian", "recap"] as const;
export type KnowerName = (typeof KNOWER_NAMES)[number];

export function validKnowersList(): string {
  return KNOWER_NAMES.join(" | ");
}

export interface RefreshRequest {
  /** a single lens; empty / undefined / "all" means every lens */
  knower?: string | null;
}

export type RefreshArgsResult =
  | { ok: true; args: string[] }
  | { ok: false; error: string };

export function refreshArgs(req: RefreshRequest = {}): RefreshArgsResult {
  const name = (req.knower ?? "").trim();
  const wantsAll = name === "" || name === "all";

  if (!wantsAll && !(KNOWER_NAMES as readonly string[]).includes(name)) {
    return { ok: false, error: `unknown knower: ${name} (valid: ${validKnowersList()} | all)` };
  }

  if (wantsAll) {
    return { ok: true, args: ["--all"] };
  }
  return { ok: true, args: ["--knower", name] };
}
