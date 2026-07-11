import { useState, useEffect } from "react";
import type { ProjectConfig } from "../types";
import { fetchConfig, updateConfig } from "../api";

export function ConfigPanel({ onClose }: { onClose: () => void }) {
  const [config, setConfig] = useState<ProjectConfig | null>(null);
  const [editing, setEditing] = useState(false);
  const [form, setForm] = useState<Record<string, string>>({});
  const [saving, setSaving] = useState(false);

  useEffect(() => {
    fetchConfig().then((c) => {
      setConfig(c);
      setForm({
        target_dir: c.daemon.target_dir ?? "",
        log_level: c.daemon.log_level ?? "info",
        window_budget_usd: String(c.budget.window_budget_usd ?? ""),
        window_hours: String(c.budget.window_hours ?? ""),
        default_max_turns: String(c.conversations.default_max_turns ?? ""),
        leader: c.conversations.leader ?? "",
        default_path: c.conversations.default_path ?? "",
      });
    });
  }, []);

  const handleSave = async () => {
    setSaving(true);
    try {
      const updates: Record<string, string | number | boolean> = {};
      for (const [key, value] of Object.entries(form)) {
        if (["window_budget_usd", "window_hours", "default_max_turns"].includes(key)) {
          const num = parseFloat(value);
          if (!isNaN(num)) updates[key] = num;
        } else {
          if (value) updates[key] = value;
        }
      }
      await updateConfig(updates);
      const refreshed = await fetchConfig();
      setConfig(refreshed);
      setEditing(false);
    } finally {
      setSaving(false);
    }
  };

  if (!config) return null;

  const Field = ({ label, field, type = "text" }: { label: string; field: string; type?: string }) => (
    <div className="flex items-center justify-between py-2 border-b border-line">
      <span className="text-muted text-sm">{label}</span>
      {editing ? (
        <input
          type={type}
          value={form[field] ?? ""}
          onChange={(e) => setForm({ ...form, [field]: e.target.value })}
          className="w-48 px-2 py-1 bg-field border border-line-soft rounded text-ink text-sm font-mono text-right focus:outline-none focus:border-line-dash"
        />
      ) : (
        <span className="text-ink text-sm font-mono">
          {form[field] || "\u2014"}
        </span>
      )}
    </div>
  );

  return (
    <div
      className="fixed inset-0 z-50 flex justify-end q-fade"
      style={{ background: "rgba(11,9,15,0.55)", backdropFilter: "blur(3px)" }}
      onClick={onClose}
    >
      <div
        className="w-96 bg-sheet border-l border-line-edge h-full overflow-y-auto q-slide"
        onClick={(e) => e.stopPropagation()}
      >
        {/* Header */}
        <div className="flex items-center justify-between px-5 py-4 border-b border-line">
          <h2 className="text-ink-bright font-semibold">Project Settings</h2>
          <div className="flex gap-2">
            {editing ? (
              <>
                <button
                  onClick={() => setEditing(false)}
                  className="px-3 py-1 border border-line-dash bg-transparent text-[#c9c3bd] rounded text-sm hover:text-ink"
                >
                  Cancel
                </button>
                <button
                  onClick={handleSave}
                  disabled={saving}
                  className="px-3 py-1 bg-brand hover:bg-brand-bright text-[#1a1410] rounded text-sm font-medium disabled:opacity-50"
                >
                  {saving ? "..." : "Save"}
                </button>
              </>
            ) : (
              <button
                onClick={() => setEditing(true)}
                className="px-3 py-1 bg-chip text-ink rounded text-sm hover:bg-line-soft"
              >
                Edit
              </button>
            )}
            <button
              onClick={onClose}
              className="ml-2 rounded border border-line-edge px-1.5 text-muted hover:text-ink"
            >
              &#x2715;
            </button>
          </div>
        </div>

        {/* Config path */}
        <div className="px-5 py-3 border-b border-line">
          <span className="text-faint text-xs font-mono">{config.config_path}</span>
        </div>

        {/* Daemon section */}
        <div className="px-5 py-3">
          <h3 className="text-faint text-xs uppercase tracking-wide mb-2">Daemon</h3>
          <Field label="Target Directory" field="target_dir" />
          <Field label="Log Level" field="log_level" />
          <div className="flex items-center justify-between py-2 border-b border-line">
            <span className="text-muted text-sm">PID File</span>
            <span className="text-faint text-sm font-mono">{config.daemon.pid_file ?? "\u2014"}</span>
          </div>
          <div className="flex items-center justify-between py-2 border-b border-line">
            <span className="text-muted text-sm">Data Directory</span>
            <span className="text-faint text-sm font-mono">{config.daemon.data_dir ?? "\u2014"}</span>
          </div>
        </div>

        {/* Budget section */}
        <div className="px-5 py-3">
          <h3 className="text-faint text-xs uppercase tracking-wide mb-2">Budget</h3>
          <Field label="Window Budget (USD)" field="window_budget_usd" type="number" />
          <Field label="Window Hours" field="window_hours" type="number" />
          {editing && (
            <p className="text-brand text-xs mt-2">Budget changes require daemon restart to take effect</p>
          )}
        </div>

        {/* Conversations section */}
        <div className="px-5 py-3">
          <h3 className="text-faint text-xs uppercase tracking-wide mb-2">Conversations</h3>
          <Field label="Leader" field="leader" />
          <Field label="Default Path" field="default_path" />
          <Field label="Max Turns" field="default_max_turns" type="number" />
        </div>

        {/* Agents section */}
        <div className="px-5 py-3">
          <h3 className="text-faint text-xs uppercase tracking-wide mb-2">Agents</h3>
          {config.agents.map((agentPath, i) => (
            <div key={i} className="py-1">
              <span className="text-muted text-sm font-mono">{agentPath}</span>
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}
