// Launch / resume the flight from the web — the operator's Model A "restart
// claude --agent supervisor" made clickable. The session is a REAL interactive
// TUI inside detached tmux (never headless -p): the button starts it, any
// terminal can attach to watch or answer gates.
import { useEffect, useState } from "react";
import { fetchFlightSession, launchFlight, type FlightSession } from "../api";
import { NIGHT } from "../lib/theme";

export function FlightLaunch({ onLaunched }: { onLaunched?: () => void }) {
  const [session, setSession] = useState<FlightSession | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [copied, setCopied] = useState(false);

  const refresh = () => fetchFlightSession().then(setSession);
  useEffect(() => {
    refresh();
    const id = setInterval(refresh, 15000);
    return () => clearInterval(id);
  }, []);

  const launch = async (e: React.MouseEvent) => {
    e.stopPropagation();
    if (busy) return;
    setBusy(true);
    setError(null);
    try {
      const res = await launchFlight();
      if (res.error && !res.started) setError(res.error);
      await refresh();
      onLaunched?.();
    } finally {
      setBusy(false);
    }
  };

  const copyAttach = async (e: React.MouseEvent) => {
    e.stopPropagation();
    if (!session?.attach) return;
    try {
      await navigator.clipboard.writeText(session.attach);
      setCopied(true);
      setTimeout(() => setCopied(false), 1600);
    } catch {}
  };

  if (!session) return null;

  if (!session.available) {
    return (
      <span className="text-[11.5px] text-faint">
        web launch unavailable (needs tmux + claude on the server's PATH)
      </span>
    );
  }

  if (session.running) {
    return (
      <span className="inline-flex flex-wrap items-center gap-2" onClick={(e) => e.stopPropagation()}>
        <span className="q-pulse font-mono text-[11.5px]" style={{ color: NIGHT }}>
          ● session live
        </span>
        <code className="rounded bg-chip px-2 py-1 font-mono text-[11px] text-ink">{session.attach}</code>
        <button
          onClick={copyAttach}
          className="rounded-md border border-line-soft px-2 py-1 font-mono text-[10.5px] text-muted hover:text-ink"
        >
          {copied ? "copied ✓" : "copy"}
        </button>
      </span>
    );
  }

  return (
    <span className="inline-flex flex-wrap items-center gap-2" onClick={(e) => e.stopPropagation()}>
      <button
        disabled={busy}
        onClick={launch}
        className="rounded-lg px-3.5 py-1.5 text-[12.5px] font-bold text-[#171319] disabled:opacity-45"
        style={{ background: NIGHT }}
        title="starts the interactive supervisor session in detached tmux — attach from any terminal"
      >
        {busy ? "launching…" : "▶ Launch flight"}
      </button>
      {error && <span className="text-[11.5px] text-closed">{error}</span>}
    </span>
  );
}
