import { useState, useEffect } from "react";
import { fetchAgentContext, updateAgentContext } from "../api";

export function AgentContextEditor({
  agentId,
  agentName,
  onClose,
}: {
  agentId: string;
  agentName: string;
  onClose: () => void;
}) {
  const [content, setContent] = useState("");
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);

  useEffect(() => {
    fetchAgentContext(agentId).then((res) => {
      setContent(res.content);
      setLoading(false);
    });
  }, [agentId]);

  const handleSave = async () => {
    setSaving(true);
    try {
      await updateAgentContext(agentId, content);
      onClose();
    } finally {
      setSaving(false);
    }
  };

  return (
    <div
      className="fixed inset-0 bg-black/60 z-50 flex justify-center items-start"
      onClick={onClose}
    >
      <div
        className="max-w-3xl w-full mx-auto mt-20 bg-zinc-950 border border-zinc-800 rounded-xl overflow-hidden"
        onClick={(e) => e.stopPropagation()}
      >
        {/* Header */}
        <div className="flex items-center justify-between px-5 py-3 border-b border-zinc-800">
          <div>
            <h2 className="text-white font-semibold text-sm">{agentName}</h2>
            <span className="text-zinc-500 text-xs font-mono">CONTEXT.md</span>
          </div>
          <button
            onClick={onClose}
            className="text-zinc-500 hover:text-zinc-300 text-lg"
          >
            &#x2715;
          </button>
        </div>

        {/* Body */}
        {loading ? (
          <div className="px-5 py-16 text-center text-zinc-500 text-sm">
            Loading...
          </div>
        ) : (
          <div className="px-5 py-4">
            <textarea
              value={content}
              onChange={(e) => setContent(e.target.value)}
              className="w-full min-h-[400px] px-4 py-3 bg-zinc-900 border border-zinc-800 rounded-lg text-sm text-zinc-300 font-mono resize-y focus:outline-none focus:border-zinc-600"
              placeholder="Write agent instructions here..."
              spellCheck={false}
            />
          </div>
        )}

        {/* Footer */}
        <div className="flex items-center justify-end gap-2 px-5 py-3 border-t border-zinc-800">
          <button
            onClick={onClose}
            className="px-4 py-1.5 text-xs bg-zinc-700 text-zinc-300 rounded hover:bg-zinc-600"
          >
            Cancel
          </button>
          <button
            onClick={handleSave}
            disabled={saving || loading}
            className="px-4 py-1.5 text-xs bg-blue-600 text-white rounded hover:bg-blue-500 disabled:opacity-50 disabled:cursor-not-allowed"
          >
            {saving ? "Saving..." : "Save"}
          </button>
        </div>
      </div>
    </div>
  );
}
