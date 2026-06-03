import { useState } from "react";
import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";
import { respondToLeader } from "../api";

export function RespondControls({
  conversationId,
  leaderMessage,
  onAction,
}: {
  conversationId: number;
  leaderMessage: string | null;
  onAction: () => void;
}) {
  const [text, setText] = useState("");
  const [loading, setLoading] = useState(false);

  const handleSend = async () => {
    if (!text.trim()) return;
    setLoading(true);
    try {
      await respondToLeader(conversationId, text);
      setText("");
      onAction();
    } finally {
      setLoading(false);
    }
  };

  return (
    <div className="w-full space-y-2">
      {leaderMessage && (
        <div className="bg-blue-950/30 border border-blue-800 rounded p-3">
          <div className="text-blue-400 text-xs font-medium mb-1">Leader asks:</div>
          <div className="md-content text-zinc-300 text-sm max-h-72 overflow-auto">
            <ReactMarkdown remarkPlugins={[remarkGfm]}>{leaderMessage}</ReactMarkdown>
          </div>
        </div>
      )}
      <textarea
        value={text}
        onChange={(e) => setText(e.target.value)}
        placeholder="Type your response to the leader..."
        className="w-full h-24 bg-zinc-950 text-zinc-300 text-sm border border-zinc-700 rounded p-2 focus:border-blue-500 focus:outline-none resize-y"
        onKeyDown={(e) => {
          if (e.key === "Enter" && (e.metaKey || e.ctrlKey)) handleSend();
        }}
      />
      <div className="flex justify-end">
        <button
          onClick={handleSend}
          disabled={loading || !text.trim()}
          className="px-4 py-1.5 bg-blue-600 text-white rounded text-sm font-medium hover:bg-blue-500 disabled:opacity-50"
        >
          Send to Leader
        </button>
      </div>
    </div>
  );
}
