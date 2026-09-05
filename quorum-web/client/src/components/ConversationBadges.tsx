import { conversationBadges, type BadgeSource } from "../lib/conversationBadges";

// Renders the daemon-persisted conversation flags beside the mode chip.
// All label/colour logic lives in lib/conversationBadges.ts (pure, tested).
export function ConversationBadges({ conversation }: { conversation: BadgeSource }) {
  const badges = conversationBadges(conversation);
  if (badges.length === 0) return null;
  return (
    <>
      {badges.map((b) => (
        <span
          key={b.key}
          title={b.title}
          className="inline-flex items-center rounded-full px-2.5 py-[2px] font-mono text-[10.5px] tracking-[0.04em]"
          style={{ border: `1px solid ${b.border}`, color: b.color, background: b.bg }}
        >
          {b.label}
        </span>
      ))}
    </>
  );
}
