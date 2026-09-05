// Pure argv builder for POST /api/knower/refresh.
//
// Mirrors the daemon's own contract in quorum-core/src/cli/knower_refresh.h:
//   :74-77  the four lenses, in order
//   :258    --all and --knower are mutually exclusive
//   :265    an unknown lens name is rejected BEFORE any project work
//   :274    --parallel requires --all  ("parallelism only makes sense across
//           the multi-lens --all set")
// Mirroring it here means the web returns 400 instead of spawning a daemon
// that will exit 1 into a detached stderr nobody reads.
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
  /** --parallel: only legal with the full --all set */
  parallel?: boolean;
}

export type RefreshArgsResult =
  | { ok: true; args: string[] }
  | { ok: false; error: string };

export function refreshArgs(req: RefreshRequest = {}): RefreshArgsResult {
  const name = (req.knower ?? "").trim();
  const wantsAll = name === "" || name === "all";
  const parallel = req.parallel === true;

  if (!wantsAll && !(KNOWER_NAMES as readonly string[]).includes(name)) {
    return { ok: false, error: `unknown knower: ${name} (valid: ${validKnowersList()} | all)` };
  }

  if (parallel && !wantsAll) {
    return { ok: false, error: "--parallel requires --all (the daemon rejects it for a single lens)" };
  }

  if (wantsAll) {
    return { ok: true, args: parallel ? ["--all", "--parallel"] : ["--all"] };
  }
  return { ok: true, args: ["--knower", name] };
}
