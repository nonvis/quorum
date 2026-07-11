// Click-don't-type at the human gate: canned one-click actions first, free
// text second. Generic gates get Approve / Proceed / Reject chips; brainstorm
// knowledge gates get the staged-note approval manifest (per-note approve/skip
// + one "Yes — save all" that maps to `quorum respond "yes"`).
import { useState } from "react";
import type { PendingVaultUpdate } from "../types";
import { DiffBlock } from "./DiffBlock";

const CHIP_BASE =
  "inline-flex items-center gap-1.5 rounded-full border px-3.5 py-1.5 text-[12.5px] font-semibold transition-colors disabled:opacity-45 disabled:cursor-not-allowed";

export function GateChips({
  onSend,
  disabled,
  compact,
}: {
  onSend: (text: string) => void;
  disabled?: boolean;
  compact?: boolean;
}) {
  const chips: { label: string; text: string; color: string; border: string; bg: string }[] = [
    {
      label: "✓ Approve",
      text: "yes — approved",
      color: "#85bd93",
      border: "rgba(133,189,147,0.4)",
      bg: "rgba(133,189,147,0.08)",
    },
    {
      label: "→ Proceed",
      text: "proceed as you proposed",
      color: "#8fa9e8",
      border: "rgba(143,169,232,0.4)",
      bg: "rgba(143,169,232,0.08)",
    },
    {
      label: "✕ Reject",
      text: "no — rejected, do not proceed",
      color: "#c98b81",
      border: "rgba(201,139,129,0.4)",
      bg: "rgba(201,139,129,0.08)",
    },
  ];
  return (
    <div className={`flex flex-wrap items-center ${compact ? "gap-1.5" : "gap-2"}`}>
      {chips.map((ch) => (
        <button
          key={ch.label}
          disabled={disabled}
          onClick={() => onSend(ch.text)}
          className={CHIP_BASE}
          style={{ color: ch.color, borderColor: ch.border, background: ch.bg }}
          title={`sends: "${ch.text}"`}
        >
          {ch.label}
        </button>
      ))}
    </div>
  );
}

// The brainstorm knowledge gate: a manifest of the staged vault notes
// (pending_vault_updates rows) with per-note approve/skip toggles. Approving
// everything sends the canonical "yes"; partial approval spells out the skips
// for the leader.
export function VaultManifest({
  rows,
  onSend,
  onDiscuss,
  disabled,
}: {
  rows: PendingVaultUpdate[];
  onSend: (text: string) => void;
  onDiscuss: () => void;
  disabled?: boolean;
}) {
  const [skipped, setSkipped] = useState<Set<number>>(new Set());
  const [openPreview, setOpenPreview] = useState<number | null>(null);

  const toggle = (id: number) =>
    setSkipped((prev) => {
      const next = new Set(prev);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });

  const approvedCount = rows.length - skipped.size;

  const submit = () => {
    if (skipped.size === 0) {
      onSend("yes");
      return;
    }
    if (approvedCount === 0) {
      onSend("no — do not save any of the staged notes.");
      return;
    }
    const skips = rows
      .filter((r) => skipped.has(r.id))
      .map((r) => `${r.agent_id}: ${r.path}`)
      .join("; ");
    onSend(`yes, but do NOT save these staged notes — skip them: ${skips}. Save the rest.`);
  };

  return (
    <div className="flex flex-col gap-2">
      <div className="font-mono text-[10px] font-bold tracking-[0.12em]" style={{ color: "#a793e6" }}>
        ◈ PROPOSED VAULT NOTES · {rows.length}
      </div>
      <div className="flex flex-col gap-1.5">
        {rows.map((r) => {
          const skip = skipped.has(r.id);
          const previewing = openPreview === r.id;
          return (
            <div
              key={r.id}
              className="overflow-hidden rounded-xl border"
              style={{
                borderColor: skip ? "#2c2834" : "rgba(167,147,230,0.35)",
                opacity: skip ? 0.55 : 1,
              }}
            >
              <div className="flex items-center gap-2.5 px-3 py-2">
                <button
                  onClick={() => toggle(r.id)}
                  disabled={disabled}
                  className="flex h-[18px] w-[18px] flex-shrink-0 items-center justify-center rounded-md border text-[11px] font-bold"
                  style={{
                    borderColor: skip ? "#3a3444" : "rgba(167,147,230,0.6)",
                    background: skip ? "transparent" : "rgba(167,147,230,0.18)",
                    color: skip ? "#5f5966" : "#c8bbef",
                  }}
                  title={skip ? "skipped — click to approve" : "approved — click to skip"}
                >
                  {skip ? "" : "✓"}
                </button>
                <span className="font-mono text-[11.5px] font-semibold" style={{ color: "#c8bbef" }}>
                  {r.agent_id}
                </span>
                <span className="min-w-0 flex-1 truncate font-mono text-[11.5px] text-muted" title={r.path}>
                  {r.path}
                </span>
                <button
                  onClick={() => setOpenPreview(previewing ? null : r.id)}
                  className="flex-shrink-0 rounded-md border border-line-soft px-2 py-0.5 font-mono text-[10.5px] text-faint hover:border-line-dash hover:text-ink"
                >
                  {previewing ? "hide" : "preview"}
                </button>
              </div>
              {previewing && <DiffBlock text={r.content} />}
            </div>
          );
        })}
      </div>
      <div className="mt-1 flex flex-wrap items-center gap-2">
        <button
          onClick={submit}
          disabled={disabled}
          className="rounded-lg px-4 py-2 text-[13px] font-bold text-[#171319] transition-opacity disabled:opacity-45"
          style={{ background: "#a793e6" }}
          title={skipped.size === 0 ? 'sends: "yes"' : "sends an approval that names the skipped notes"}
        >
          {skipped.size === 0
            ? `Yes — save all${rows.length > 1 ? ` (${rows.length})` : ""}`
            : approvedCount === 0
              ? "Skip all — save nothing"
              : `Save ${approvedCount}, skip ${skipped.size}`}
        </button>
        <button
          onClick={onDiscuss}
          disabled={disabled}
          className={CHIP_BASE}
          style={{ color: "#8a8390", borderColor: "#2c2834", background: "transparent" }}
        >
          No — keep discussing…
        </button>
      </div>
    </div>
  );
}
