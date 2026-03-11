import { useState } from "react";
import { gateApprove, gateReject } from "../api";

export function GateControls({
  conversationId,
  onAction,
}: {
  conversationId: number;
  onAction: () => void;
}) {
  const [loading, setLoading] = useState(false);

  const handleApprove = async () => {
    setLoading(true);
    try {
      await gateApprove(conversationId);
      onAction();
    } finally {
      setLoading(false);
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

  return (
    <div className="flex gap-2">
      <button
        onClick={handleApprove}
        disabled={loading}
        className="px-3 py-1 bg-green-600 text-white rounded text-sm font-medium hover:bg-green-500 disabled:opacity-50"
      >
        Approve
      </button>
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
