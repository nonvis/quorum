import { getConversations, type Conversation } from "./db";

// Poll interval in ms
const POLL_INTERVAL = 2000;

function getSnapshot(): Map<number, string> {
  const convs = getConversations();
  const snap = new Map<number, string>();
  for (const c of convs) {
    // Include state + round + spent + current_agent so we detect mid-conversation changes
    snap.set(c.id, `${c.state}|${c.round}|${c.spent_usd}|${c.current_agent}`);
  }
  return snap;
}

// Create SSE stream for a client
export function createSSEStream(): ReadableStream {
  let interval: Timer;
  // Per-stream change-detection snapshot (NOT shared across clients — a module
  // global races between concurrent/reconnecting streams and drops updates).
  let lastSnapshot = new Map<number, string>();

  return new ReadableStream({
    start(controller) {
      // Send initial snapshot
      const convs = getConversations();
      const data = JSON.stringify({ type: "snapshot", conversations: convs });
      controller.enqueue(`data: ${data}\n\n`);
      lastSnapshot = getSnapshot();

      interval = setInterval(async () => {
        try {
          // Detect changes
          const current = getSnapshot();
          const changes: Array<{ id: number; state: string; prev: string | undefined }> = [];

          for (const [id, state] of current) {
            if (lastSnapshot.get(id) !== state) {
              changes.push({ id, state, prev: lastSnapshot.get(id) });
            }
          }
          // New conversations
          for (const [id, state] of current) {
            if (!lastSnapshot.has(id)) {
              changes.push({ id, state, prev: undefined });
            }
          }

          if (changes.length > 0) {
            const convs = getConversations();
            const data = JSON.stringify({ type: "update", changes, conversations: convs });
            controller.enqueue(`data: ${data}\n\n`);
          } else {
            // Keepalive to prevent idle timeout
            controller.enqueue(`: keepalive\n\n`);
          }

          lastSnapshot = current;
        } catch (e) {
          // Client disconnected or DB error — will be caught by close
        }
      }, POLL_INTERVAL);
    },
    cancel() {
      clearInterval(interval);
    },
  });
}
