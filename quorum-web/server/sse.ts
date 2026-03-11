import { getConversations, type Conversation } from "./db";
import { execDaemon } from "./daemon";

// Track auto-approve conversations (in-memory)
const autoApproveSet = new Set<number>();

export function markAutoApprove(conversationId: number) {
  autoApproveSet.add(conversationId);
}

// Poll interval in ms
const POLL_INTERVAL = 2000;

// Track last known state per conversation for change detection
let lastSnapshot: Map<number, string> = new Map();

function getSnapshot(): Map<number, string> {
  const convs = getConversations();
  const snap = new Map<number, string>();
  for (const c of convs) {
    snap.set(c.id, c.state);
  }
  return snap;
}

// Check for auto-approve candidates and execute
async function checkAutoApprove() {
  for (const convId of autoApproveSet) {
    const convs = getConversations();
    const conv = convs.find((c) => c.id === convId);
    if (!conv) {
      autoApproveSet.delete(convId);
      continue;
    }
    if (conv.state === "approved") {
      console.log(`[auto-approve] conversation ${convId} — executing gate --approve`);
      const result = await execDaemon("gate", "--approve", "--conversation", String(convId));
      if (result.success) {
        console.log(`[auto-approve] conversation ${convId} — approved`);
      } else {
        console.error(`[auto-approve] conversation ${convId} — failed: ${result.stderr}`);
      }
      autoApproveSet.delete(convId);
    }
    // Clean up if conversation is terminal
    if (["done", "closed"].includes(conv.state)) {
      autoApproveSet.delete(convId);
    }
  }
}

// Create SSE stream for a client
export function createSSEStream(): ReadableStream {
  let interval: Timer;

  return new ReadableStream({
    start(controller) {
      // Send initial snapshot
      const convs = getConversations();
      const data = JSON.stringify({ type: "snapshot", conversations: convs });
      controller.enqueue(`data: ${data}\n\n`);
      lastSnapshot = getSnapshot();

      interval = setInterval(async () => {
        try {
          // Check auto-approve
          await checkAutoApprove();

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
