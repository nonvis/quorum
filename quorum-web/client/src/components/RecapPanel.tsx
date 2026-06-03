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
      className="fixed inset-0 bg-black/60 z-50 flex justify-center items-start"
      onClick={onClose}
    >
      <div
        className="max-w-3xl w-full mx-auto mt-20 bg-zinc-950 border border-zinc-800 rounded-xl overflow-hidden flex flex-col max-h-[80vh]"
        onClick={(e) => e.stopPropagation()}
      >
        {/* Header */}
        <div className="flex items-center justify-between px-5 py-3 border-b border-zinc-800">
          <div>
            <h2 className="text-white font-semibold text-sm">What's going on?</h2>
            <span className="text-zinc-500 text-xs">
              On-demand recap (quorum ask --agent recap)
            </span>
          </div>
          <button
            onClick={onClose}
            className="text-zinc-500 hover:text-zinc-300 text-lg"
          >
            &#x2715;
          </button>
        </div>

        {/* Controls */}
        <div className="px-5 py-4 border-b border-zinc-800 space-y-3">
          <div className="flex flex-wrap gap-2">
            {CANNED.map((c) => (
              <button
                key={c.label}
                type="button"
                onClick={() => run(c.prompt)}
                disabled={loading}
                title={c.prompt}
                className="px-3 py-1.5 text-xs bg-zinc-800 text-zinc-200 rounded-lg hover:bg-zinc-700 disabled:opacity-50 disabled:cursor-not-allowed"
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
              className="flex-1 px-4 py-2 bg-zinc-800 border border-zinc-700 rounded-lg text-sm text-white placeholder-zinc-500 focus:outline-none focus:border-zinc-500 disabled:opacity-50"
            />
            <button
              type="submit"
              disabled={loading || !freeText.trim()}
              className="px-4 py-2 text-sm bg-blue-600 text-white rounded-lg font-medium hover:bg-blue-500 disabled:opacity-50 disabled:cursor-not-allowed"
            >
              Ask
            </button>
          </form>
        </div>

        {/* Answer */}
        <div className="px-5 py-4 overflow-y-auto">
          {askedPrompt && (
            <p className="text-xs text-zinc-500 mb-3">
              <span className="text-zinc-400">Asked:</span> {askedPrompt}
            </p>
          )}
          {loading && (
            <div className="text-zinc-500 text-sm">
              Asking the recap agent… (this can take a few minutes)
            </div>
          )}
          {!loading && error && (
            <div className="px-4 py-3 bg-red-900/30 border border-red-800 rounded-lg text-red-400 text-sm whitespace-pre-wrap">
              {error}
            </div>
          )}
          {!loading && !error && answer != null && (
            <div className="prose prose-invert prose-sm max-w-none text-sm text-zinc-300">
              <ReactMarkdown remarkPlugins={[remarkGfm]}>{answer}</ReactMarkdown>
            </div>
          )}
          {!loading && !error && answer == null && (
            <p className="text-zinc-600 text-sm">
              Pick a prompt above or ask a question to get a recap of the active
              project.
            </p>
          )}
        </div>
      </div>
    </div>
  );
}
