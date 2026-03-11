import { useEffect, useRef } from "react";
import type { Conversation } from "../types";

interface SSEMessage {
  type: "snapshot" | "update";
  conversations: Conversation[];
}

export function useSSE(onUpdate: (conversations: Conversation[]) => void) {
  const eventSourceRef = useRef<EventSource | null>(null);

  useEffect(() => {
    const es = new EventSource("/api/events");
    eventSourceRef.current = es;

    es.onmessage = (event) => {
      try {
        const data: SSEMessage = JSON.parse(event.data);
        onUpdate(data.conversations);
      } catch {
        // ignore parse errors
      }
    };

    es.onerror = () => {
      // EventSource auto-reconnects
    };

    return () => {
      es.close();
    };
  }, []); // onUpdate intentionally excluded — use ref or stable callback
}
