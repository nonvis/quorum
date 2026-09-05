// Chips for the conversation flags the daemon already persists and the API
// already returns (`SELECT *` in server/db.ts) but nothing rendered:
// no_vault_write, gated, gate_cleared, team.
//
// Pure — the component layer only maps these to spans, so the label/colour
// contract is unit-testable without a DOM.
//
// Palette rule: amber (--color-brand) is the HUMAN-GATE colour and nothing
// else. Only `gated · pending` — a conversation actually waiting on a person —
// may wear it.

export interface ConversationBadge {
  key: string;
  label: string;
  /** text colour */
  color: string;
  /** chip background */
  bg: string;
  /** chip border */
  border: string;
  /** hover explanation */
  title: string;
}

export interface BadgeSource {
  no_vault_write?: number;
  gated?: number;
  gate_cleared?: number;
  team?: string | null;
}

export const BADGE_AMBER = "#e3a45c";

export function conversationBadges(conv: BadgeSource): ConversationBadge[] {
  const out: ConversationBadge[] = [];

  if (conv.no_vault_write === 1) {
    out.push({
      key: "no_vault_write",
      label: "read-only vault",
      color: "#a793e6",
      bg: "rgba(167,147,230,0.10)",
      border: "rgba(167,147,230,0.35)",
      title: "vault writes are suppressed for this conversation",
    });
  }

  if (conv.gated === 1) {
    const cleared = conv.gate_cleared === 1;
    out.push(
      cleared
        ? {
            key: "gate_cleared",
            label: "gated · cleared",
            color: "#85bd93",
            bg: "rgba(133,189,147,0.10)",
            border: "rgba(133,189,147,0.35)",
            title: "the human gate has been answered — staged writes released",
          }
        : {
            key: "gated",
            label: "gated · pending",
            color: BADGE_AMBER,
            bg: "rgba(227,164,92,0.10)",
            border: "rgba(227,164,92,0.40)",
            title: "staged vault writes are held until you approve",
          },
    );
  }

  const team = conv.team?.trim();
  if (team) {
    out.push({
      key: "team",
      label: `team: ${team}`,
      color: "#8a8390",
      bg: "rgba(138,131,144,0.10)",
      border: "rgba(138,131,144,0.35)",
      title: "agent team this conversation was started with",
    });
  }

  return out;
}
