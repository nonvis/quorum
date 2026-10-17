const KNOWN = ['host', 'builder', 'verifier', 'architect', 'product', 'operator'];

/** Each specialty's colour, a CSS variable (index.css); specialties added later share one. */
export function speakerColor(id: string): string {
  return `var(--spk-${KNOWN.includes(id) ? id : 'other'})`;
}

const PAN_X: Record<string, number> = {
  'far left': -0.9,
  left: -0.6,
  'centre-left': -0.3,
  centre: 0,
  'centre-right': 0.3,
  right: 0.6,
  'far right': 0.9,
};

export function panX(pan: string): number {
  return PAN_X[pan] ?? 0;
}
