import { test, expect } from "bun:test";
import { deriveVerdict } from "./verdict";

// The daemon clips a summary at 200 bytes, on a word boundary, and appends its
// own "…". Anything between 181 and 200 is therefore a summary the client used
// to cut a SECOND time, mid-word, and end with a second ellipsis.
function proseOfLength(n: number): string {
  const base = "the invoker builds the claude -p command line and hands it to the brain ";
  let s = "";
  while (s.length < n) s += base;
  s = s.slice(0, n);
  return s.endsWith(" ") ? s.slice(0, -1) + "x" : s;
}

/** A realistic daemon summary: 194 chars of prose + the daemon's own ellipsis. */
const DAEMON_SUMMARY = proseOfLength(194) + "…";
/** A 195-char first sentence the CLIENT has to derive out of free prose. */
const DERIVED_SENTENCE = proseOfLength(194) + ".";

const base = { result: null as string | null, error: null as string | null, status: "done" };

test("the fixtures are the lengths the claims are about", () => {
  expect(DAEMON_SUMMARY.length).toBe(195);
  expect(DERIVED_SENTENCE.length).toBe(195);
});

test("a 195-char daemon summary renders verbatim — the daemon's clip is the clip", () => {
  const v = deriveVerdict({ ...base, summary: DAEMON_SUMMARY });
  expect(v.text).toBe(DAEMON_SUMMARY);
  expect(v.text.length).toBe(195);
  // one ellipsis, the daemon's — not a second one stacked at 180
  expect(v.text.split("…")).toHaveLength(2);
  expect(v.kind).toBe("verdict");
  expect(v.explicit).toBe(true);
});

test("a 195-char DERIVED first sentence is still clipped to 180", () => {
  const v = deriveVerdict({ ...base, result: DERIVED_SENTENCE });
  expect(v.text.length).toBe(180);
  expect(v.text.endsWith("…")).toBe(true);
  expect(v.text).toBe(DERIVED_SENTENCE.slice(0, 179) + "…");
  expect(v.explicit).toBe(false);
});

test("a 195-char derived VERDICT: line is clipped too — the summary field is the only exemption", () => {
  const v = deriveVerdict({ ...base, result: `VERDICT: ${DERIVED_SENTENCE}\n\nfull prose follows.` });
  expect(v.text.length).toBe(180);
  expect(v.text.endsWith("…")).toBe(true);
  expect(v.explicit).toBe(true);
});

test("a daemon summary is still normalized to one line", () => {
  const v = deriveVerdict({ ...base, summary: "  the box is up\n   and serving   HTTPS  " });
  expect(v.text).toBe("the box is up and serving HTTPS");
});

test("a short daemon summary is untouched, and still beats the result prose", () => {
  const v = deriveVerdict({ ...base, summary: "quota denied: 12 vCPU global.", result: "VERDICT: something else." });
  expect(v.text).toBe("quota denied: 12 vCPU global.");
});
