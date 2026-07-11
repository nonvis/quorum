// "Ask Docent" — the knowledge agent's web surface. Where recap re-reads the
// live project over minutes, Docent answers from the accumulated knower vaults
// in seconds, with per-claim [note path] citations or an explicit refusal.
// Backed by quorum-own-agent/ (see its README + the vault's Quorum/Docent notes).
import { useEffect, useState } from "react";
import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";
import { askDocent, fetchDocentHistory, type DocentHistoryItem } from "../api";

export function DocentPanel({ onClose }: { onClose: () => void }) {
  const [question, setQuestion] = useState("");
  const [loading, setLoading] = useState(false);
  const [asked, setAsked] = useState<string | null>(null);
  const [answer, setAnswer] = useState<string | null>(null);
  const [steps, setSteps] = useState<string[]>([]);
  const [error, setError] = useState<string | null>(null);
  // Answers don't evaporate: the transcript bank doubles as panel memory.
  const [history, setHistory] = useState<DocentHistoryItem[]>([]);

  useEffect(() => {
    fetchDocentHistory().then(setHistory);
  }, []);

  const showHistoryItem = (h: DocentHistoryItem) => {
    if (loading) return;
    setAsked(h.question);
    setAnswer(h.answer);
    setSteps([]);
    setError(null);
  };

  const run = async (e: React.FormEvent) => {
    e.preventDefault();
    const q = question.trim();
    if (!q || loading) return;
    setLoading(true);
    setError(null);
    setAnswer(null);
    setSteps([]);
    setAsked(q);
    try {
      const res = await askDocent(q);
      if (res.error) setError(res.error);
      else {
        setAnswer(res.answer ?? "");
        setSteps(res.steps ?? []);
        fetchDocentHistory().then(setHistory);
      }
    } catch (err) {
      setError(err instanceof Error ? err.message : "request failed");
    } finally {
      setLoading(false);
    }
  };

  return (
    <div
      className="q-fade fixed inset-0 z-50 flex items-start justify-center"
      style={{ background: "rgba(11,9,15,0.55)", backdropFilter: "blur(3px)" }}
      onClick={onClose}
    >
      <div
        className="mx-auto mt-20 flex max-h-[80vh] w-full max-w-3xl flex-col overflow-hidden rounded-xl border border-line-edge bg-sheet"
        onClick={(e) => e.stopPropagation()}
      >
        <div className="flex items-center justify-between border-b border-line px-5 py-3">
          <div>
            <h2 className="text-sm font-semibold text-ink-bright">Ask Docent</h2>
            <span className="text-xs text-faint">
              answers from the knower vaults — seconds, cited, or an honest refusal
            </span>
          </div>
          <button
            onClick={onClose}
            className="rounded border border-line-edge px-1.5 text-lg text-muted hover:text-ink"
          >
            ✕
          </button>
        </div>

        <form onSubmit={run} className="flex gap-2 border-b border-line px-5 py-4">
          <input
            type="text"
            value={question}
            onChange={(e) => setQuestion(e.target.value)}
            placeholder="What does the team know about…?"
            disabled={loading}
            autoFocus
            className="flex-1 rounded-lg border border-line-soft bg-field px-4 py-2 text-sm text-ink placeholder-dim outline-none focus:border-line-dash disabled:opacity-50"
          />
          <button
            type="submit"
            disabled={loading || !question.trim()}
            className="rounded-lg px-4 py-2 text-sm font-bold text-[#171319] disabled:cursor-not-allowed disabled:opacity-50"
            style={{ background: "#63b3a6" }}
          >
            Ask
          </button>
        </form>

        <div className="overflow-y-auto px-5 py-4">
          {asked && (
            <p className="mb-3 text-xs text-faint">
              <span className="text-muted">Asked:</span> {asked}
            </p>
          )}
          {loading && (
            <div className="text-sm text-faint">
              <span className="q-pulse">Docent is working the knowledge base…</span> (typically 15–40s)
            </div>
          )}
          {!loading && error && (
            <div
              className="whitespace-pre-wrap rounded-lg px-4 py-3 text-sm text-closed"
              style={{ background: "rgba(201,139,129,0.08)", border: "1px solid rgba(201,139,129,0.35)" }}
            >
              {error}
            </div>
          )}
          {!loading && !error && answer != null && (
            <>
              {steps.length > 0 && (
                <div className="mb-3 flex flex-col gap-0.5">
                  {steps.map((s, i) => (
                    <span key={i} className="font-mono text-[11px] text-dim">
                      {s}
                    </span>
                  ))}
                </div>
              )}
              <div className="md-content">
                <ReactMarkdown remarkPlugins={[remarkGfm]}>{answer}</ReactMarkdown>
              </div>
            </>
          )}
          {!loading && !error && answer == null && (
            <p className="text-sm text-faint">
              Ask anything the knowers have banked — decisions, architecture, where
              things live. Every claim comes cited with its note path; if the
              knowledge base doesn't cover it, Docent says so.
            </p>
          )}

          {history.length > 0 && (
            <div className="mt-5 border-t border-line pt-3">
              <div className="mb-2 font-mono text-[10.5px] font-bold tracking-[0.12em] text-faint">
                RECENT · {history.length}
              </div>
              <div className="flex flex-col gap-1">
                {history.map((h, i) => (
                  <button
                    key={`${h.ts}-${i}`}
                    onClick={() => showHistoryItem(h)}
                    disabled={loading}
                    className={`flex items-baseline gap-2.5 rounded-lg px-2 py-1.5 text-left hover:bg-chip disabled:opacity-50 ${
                      asked === h.question ? "bg-chip" : ""
                    }`}
                  >
                    <span className="flex-shrink-0 font-mono text-[10.5px] text-dim">
                      {h.ts.slice(5, 16).replace("T", " ")}
                    </span>
                    <span className="line-clamp-1 flex-1 text-[12.5px] text-muted">{h.question}</span>
                    <span className="flex-shrink-0 font-mono text-[10px] text-dim">
                      {h.mode === "agentic" ? `${h.steps} step${h.steps === 1 ? "" : "s"}` : "1-shot"}
                    </span>
                  </button>
                ))}
              </div>
            </div>
          )}
        </div>
      </div>
    </div>
  );
}
