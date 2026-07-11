import { useState } from "react";
import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";
import { askRecap } from "../api";

// Phase 14 T6 — "What's going on?" recap popup.
//
// Replaces the retired librarian's curated human docs with on-demand recap:
// each action shells `quorum ask --agent recap "<prompt>"` against the active
// project (server-side) and renders the answer. Two canned prompts (OQ3) +
// a free-text box. The recap knower must be set up in the project; the server
// surfaces the CLI's error (e.g. not set up) which we show inline.

const CANNED: { label: string; prompt: string }[] = [
  {
    label: "Recent activity / what changed",
    prompt: "Summarize recent activity and what changed lately",
  },
  {
    label: "Where we left off / next steps",
    prompt: "Where did we leave off — what are the next steps?",
  },
];

export function RecapPanel({ onClose }: { onClose: () => void }) {
  const [freeText, setFreeText] = useState("");
  const [loading, setLoading] = useState(false);
  const [askedPrompt, setAskedPrompt] = useState<string | null>(null);
  const [answer, setAnswer] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  const run = async (prompt: string) => {
    if (loading || !prompt.trim()) return;
    setLoading(true);
    setError(null);
    setAnswer(null);
    setAskedPrompt(prompt.trim());
    try {
      const res = await askRecap(prompt.trim());
      if (res.error) {
        setError(res.error);
      } else {
        setAnswer(res.answer ?? "");
      }
    } catch (e) {
      setError(e instanceof Error ? e.message : "request failed");
    } finally {
      setLoading(false);
    }
  };

  const handleFreeText = (e: React.FormEvent) => {
    e.preventDefault();
    run(freeText);
  };

  return (
    <div
      className="fixed inset-0 z-50 flex justify-center items-start q-fade"
      style={{ background: "rgba(11,9,15,0.55)", backdropFilter: "blur(3px)" }}
      onClick={onClose}
    >
      <div
        className="max-w-3xl w-full mx-auto mt-20 bg-sheet border border-line-edge rounded-xl overflow-hidden flex flex-col max-h-[80vh]"
        onClick={(e) => e.stopPropagation()}
      >
        {/* Header */}
        <div className="flex items-center justify-between px-5 py-3 border-b border-line">
          <div>
            <h2 className="text-ink-bright font-semibold text-sm">What's going on?</h2>
            <span className="text-faint text-xs">
              On-demand recap (quorum ask --agent recap)
            </span>
          </div>
          <button
            onClick={onClose}
            className="rounded border border-line-edge px-1.5 text-lg text-muted hover:text-ink"
          >
            &#x2715;
          </button>
        </div>

        {/* Controls */}
        <div className="px-5 py-4 border-b border-line space-y-3">
          <div className="flex flex-wrap gap-2">
            {CANNED.map((c) => (
              <button
                key={c.label}
                type="button"
                onClick={() => run(c.prompt)}
                disabled={loading}
                title={c.prompt}
                className="px-3 py-1.5 text-xs bg-chip text-ink rounded-lg hover:bg-line-soft disabled:opacity-50 disabled:cursor-not-allowed"
              >
                {c.label}
              </button>
            ))}
          </div>
          <form onSubmit={handleFreeText} className="flex gap-2">
            <input
              type="text"
              value={freeText}
              onChange={(e) => setFreeText(e.target.value)}
              placeholder="Ask the recap agent anything..."
              disabled={loading}
              className="flex-1 px-4 py-2 bg-field border border-line-soft rounded-lg text-sm text-ink placeholder-dim focus:outline-none focus:border-line-dash disabled:opacity-50"
            />
            <button
              type="submit"
              disabled={loading || !freeText.trim()}
              className="px-4 py-2 text-sm bg-brand hover:bg-brand-bright text-[#1a1410] rounded-lg font-medium disabled:opacity-50 disabled:cursor-not-allowed"
            >
              Ask
            </button>
          </form>
        </div>

        {/* Answer */}
        <div className="px-5 py-4 overflow-y-auto">
          {askedPrompt && (
            <p className="text-xs text-faint mb-3">
              <span className="text-muted">Asked:</span> {askedPrompt}
            </p>
          )}
          {loading && (
            <div className="text-faint text-sm">
              Asking the recap agent… (this can take a few minutes)
            </div>
          )}
          {!loading && error && (
            <div
              className="px-4 py-3 rounded-lg text-closed text-sm whitespace-pre-wrap"
              style={{ background: "rgba(201,139,129,0.08)", border: "1px solid rgba(201,139,129,0.35)" }}
            >
              {error}
            </div>
          )}
          {!loading && !error && answer != null && (
            <div className="prose prose-invert prose-sm max-w-none text-sm text-muted">
              <ReactMarkdown remarkPlugins={[remarkGfm]}>{answer}</ReactMarkdown>
            </div>
          )}
          {!loading && !error && answer == null && (
            <p className="text-faint text-sm">
              Pick a prompt above or ask a question to get a recap of the active
              project.
            </p>
          )}
        </div>
      </div>
    </div>
  );
}
