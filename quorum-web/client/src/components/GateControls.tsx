import { useState } from "react";
import { gateApprove, gateReject } from "../api";

export function GateControls({
  conversationId,
  proposalText,
  onAction,
}: {
  conversationId: number;
  proposalText?: string | null;
  onAction: () => void;
}) {
  const [loading, setLoading] = useState(false);
  const [editing, setEditing] = useState(false);
  const [editedProposal, setEditedProposal] = useState(proposalText ?? "");

  const handleApprove = async () => {
    setLoading(true);
    try {
      await gateApprove(conversationId);
      onAction();
    } finally {
      setLoading(false);
    }
  };

  const handleEditApprove = async () => {
    setLoading(true);
    try {
      await gateApprove(conversationId, editedProposal);
      onAction();
    } finally {
      setLoading(false);
      setEditing(false);
    }
  };

  const handleReject = async () => {
    setLoading(true);
    try {
      await gateReject(conversationId);
      onAction();
    } finally {
      setLoading(false);
    }
  };

  if (editing) {
    return (
      <div className="w-full space-y-2">
        <textarea
          value={editedProposal}
          onChange={(e) => setEditedProposal(e.target.value)}
          className="w-full h-64 bg-zinc-950 text-zinc-300 text-xs font-mono border border-zinc-700 rounded p-2 focus:border-blue-500 focus:outline-none resize-y"
        />
        <div className="flex gap-2 justify-end">
          <button
            onClick={() => setEditing(false)}
            disabled={loading}
            className="px-3 py-1 text-zinc-400 border border-zinc-700 rounded text-sm hover:bg-zinc-800 disabled:opacity-50"
          >
            Cancel
          </button>
          <button
            onClick={handleEditApprove}
            disabled={loading}
            className="px-3 py-1 bg-blue-600 text-white rounded text-sm font-medium hover:bg-blue-500 disabled:opacity-50"
          >
            Save & Approve
          </button>
        </div>
      </div>
    );
  }

  return (
    <div className="flex gap-2">
      <button
        onClick={handleApprove}
        disabled={loading}
        className="px-3 py-1 bg-green-600 text-white rounded text-sm font-medium hover:bg-green-500 disabled:opacity-50"
      >
        Approve
      </button>
      {proposalText && (
        <button
          onClick={() => {
            setEditedProposal(proposalText);
            setEditing(true);
          }}
          disabled={loading}
          className="px-3 py-1 bg-zinc-700 text-white rounded text-sm font-medium hover:bg-zinc-600 disabled:opacity-50"
        >
          Edit
        </button>
      )}
      <button
        onClick={handleReject}
        disabled={loading}
        className="px-3 py-1 bg-red-600 text-white rounded text-sm font-medium hover:bg-red-500 disabled:opacity-50"
      >
        Reject
      </button>
    </div>
  );
}
