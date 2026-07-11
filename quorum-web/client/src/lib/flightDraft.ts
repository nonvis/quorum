// Goal-first flight drafting: turn one high-level goal into an editable
// major-task list, deterministically (no tokens, instant). The operator
// edits/reorders/removes before the plan is generated — this is a starting
// point, not a planner. (A model-drafted task list can slot in behind the
// same signature later.)

const MAX_TASKS = 8;

function tidy(s: string): string {
  const t = s.trim().replace(/[.;,]$/, "").trim();
  return t.charAt(0).toUpperCase() + t.slice(1);
}

export function draftTasks(goal: string): string[] {
  // 1. Explicit structure wins: numbered or bulleted lines.
  const lines = goal.split("\n").map((l) => l.trim());
  const bullets = lines
    .filter((l) => /^(\d+[.)]\s+|[-*•]\s+)/.test(l))
    .map((l) => l.replace(/^(\d+[.)]\s+|[-*•]\s+)/, ""));
  if (bullets.length >= 2) return bullets.slice(0, MAX_TASKS).map(tidy);

  const flat = goal.replace(/\s+/g, " ").trim();

  // 2. Sequenced clauses: "A, then B; C → D".
  const seq = flat
    .split(/\s*(?:;|→|\bthen\b|\band then\b|\bafter that\b)\s*/i)
    .map((s) => s.trim())
    .filter((s) => s.length > 8);
  if (seq.length >= 2) return seq.slice(0, MAX_TASKS).map(tidy);

  // 3. Sentences.
  const sentences = flat
    .split(/(?<=[.!?])\s+/)
    .map((s) => s.trim())
    .filter((s) => s.length > 12);
  if (sentences.length >= 2) return sentences.slice(0, MAX_TASKS).map(tidy);

  // 4. One task = the goal itself.
  return flat ? [tidy(flat)] : [];
}
