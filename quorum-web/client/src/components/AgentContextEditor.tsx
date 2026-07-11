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
      className="fixed inset-0 z-50 flex justify-center items-start q-fade"
      style={{ background: "rgba(11,9,15,0.55)", backdropFilter: "blur(3px)" }}
      onClick={onClose}
    >
      <div
        className="max-w-3xl w-full mx-auto mt-20 bg-sheet border border-line-edge rounded-xl overflow-hidden"
        onClick={(e) => e.stopPropagation()}
      >
        {/* Header */}
        <div className="flex items-center justify-between px-5 py-3 border-b border-line">
          <div>
            <h2 className="text-ink-bright font-semibold text-sm">{agentName}</h2>
            <span className="text-faint text-xs font-mono">CONTEXT.md</span>
          </div>
          <button
            onClick={onClose}
            className="rounded border border-line-edge px-1.5 text-lg text-muted hover:text-ink"
          >
            &#x2715;
          </button>
        </div>

        {/* Body */}
        {loading ? (
          <div className="px-5 py-16 text-center text-faint text-sm">
            Loading...
          </div>
        ) : (
          <div className="px-5 py-4">
            <textarea
              value={content}
              onChange={(e) => setContent(e.target.value)}
              className="w-full min-h-[400px] px-4 py-3 bg-field border border-line-soft rounded-lg text-sm text-ink font-mono resize-y focus:outline-none focus:border-line-dash"
              placeholder="Write agent instructions here..."
              spellCheck={false}
            />
          </div>
        )}

        {/* Footer */}
        <div className="flex items-center justify-end gap-2 px-5 py-3 border-t border-line">
          <button
            onClick={onClose}
            className="px-4 py-1.5 text-xs border border-line-dash bg-transparent text-[#c9c3bd] rounded hover:text-ink"
          >
            Cancel
          </button>
          <button
            onClick={handleSave}
            disabled={saving || loading}
            className="px-4 py-1.5 text-xs bg-brand hover:bg-brand-bright text-[#1a1410] rounded font-medium disabled:opacity-50 disabled:cursor-not-allowed"
          >
            {saving ? "Saving..." : "Save"}
          </button>
        </div>
      </div>
    </div>
  );
}
