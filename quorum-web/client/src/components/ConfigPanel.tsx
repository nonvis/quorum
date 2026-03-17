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
        daily_limit_usd: String(c.budget.daily_limit_usd ?? ""),
        hourly_limit_usd: String(c.budget.hourly_limit_usd ?? ""),
        task_timeout_seconds: String(c.budget.task_timeout_seconds ?? ""),
        default_budget_usd: String(c.conversations.default_budget_usd ?? ""),
        default_max_rounds: String(c.conversations.default_max_rounds ?? ""),
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
        if (["daily_limit_usd", "hourly_limit_usd", "task_timeout_seconds", "default_budget_usd", "default_max_rounds"].includes(key)) {
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
    <div className="flex items-center justify-between py-2 border-b border-zinc-800">
      <span className="text-zinc-400 text-sm">{label}</span>
      {editing ? (
        <input
          type={type}
          value={form[field] ?? ""}
          onChange={(e) => setForm({ ...form, [field]: e.target.value })}
          className="w-48 px-2 py-1 bg-zinc-800 border border-zinc-700 rounded text-white text-sm font-mono text-right focus:outline-none focus:border-zinc-500"
        />
      ) : (
        <span className="text-white text-sm font-mono">
          {form[field] || "\u2014"}
        </span>
      )}
    </div>
  );

  return (
    <div className="fixed inset-0 bg-black/60 z-50 flex justify-end" onClick={onClose}>
      <div
        className="w-96 bg-zinc-900 border-l border-zinc-800 h-full overflow-y-auto"
        onClick={(e) => e.stopPropagation()}
      >
        {/* Header */}
        <div className="flex items-center justify-between px-5 py-4 border-b border-zinc-800">
          <h2 className="text-white font-semibold">Project Settings</h2>
          <div className="flex gap-2">
            {editing ? (
              <>
                <button
                  onClick={() => setEditing(false)}
                  className="px-3 py-1 text-zinc-400 border border-zinc-700 rounded text-sm hover:bg-zinc-800"
                >
                  Cancel
                </button>
                <button
                  onClick={handleSave}
                  disabled={saving}
                  className="px-3 py-1 bg-blue-600 text-white rounded text-sm font-medium hover:bg-blue-500 disabled:opacity-50"
                >
                  {saving ? "..." : "Save"}
                </button>
              </>
            ) : (
              <button
                onClick={() => setEditing(true)}
                className="px-3 py-1 bg-zinc-700 text-white rounded text-sm hover:bg-zinc-600"
              >
                Edit
              </button>
            )}
            <button
              onClick={onClose}
              className="text-zinc-500 hover:text-zinc-300 ml-2"
            >
              &#x2715;
            </button>
          </div>
        </div>

        {/* Config path */}
        <div className="px-5 py-3 border-b border-zinc-800">
          <span className="text-zinc-500 text-xs font-mono">{config.config_path}</span>
        </div>

        {/* Daemon section */}
        <div className="px-5 py-3">
          <h3 className="text-zinc-500 text-xs uppercase tracking-wide mb-2">Daemon</h3>
          <Field label="Target Directory" field="target_dir" />
          <Field label="Log Level" field="log_level" />
          <div className="flex items-center justify-between py-2 border-b border-zinc-800">
            <span className="text-zinc-400 text-sm">PID File</span>
            <span className="text-zinc-500 text-sm font-mono">{config.daemon.pid_file ?? "\u2014"}</span>
          </div>
          <div className="flex items-center justify-between py-2 border-b border-zinc-800">
            <span className="text-zinc-400 text-sm">Data Directory</span>
            <span className="text-zinc-500 text-sm font-mono">{config.daemon.data_dir ?? "\u2014"}</span>
          </div>
        </div>

        {/* Budget section */}
        <div className="px-5 py-3">
          <h3 className="text-zinc-500 text-xs uppercase tracking-wide mb-2">Budget</h3>
          <Field label="Daily Limit (USD)" field="daily_limit_usd" type="number" />
          <Field label="Hourly Limit (USD)" field="hourly_limit_usd" type="number" />
          <Field label="Task Timeout (sec)" field="task_timeout_seconds" type="number" />
          {editing && (
            <p className="text-amber-500 text-xs mt-2">Budget changes require daemon restart to take effect</p>
          )}
        </div>

        {/* Conversations section */}
        <div className="px-5 py-3">
          <h3 className="text-zinc-500 text-xs uppercase tracking-wide mb-2">Conversations</h3>
          <Field label="Leader" field="leader" />
          <Field label="Default Path" field="default_path" />
          <Field label="Default Budget (USD)" field="default_budget_usd" type="number" />
          <Field label="Max Rounds" field="default_max_rounds" type="number" />
        </div>

        {/* Agents section */}
        <div className="px-5 py-3">
          <h3 className="text-zinc-500 text-xs uppercase tracking-wide mb-2">Agents</h3>
          {config.agents.map((agentPath, i) => (
            <div key={i} className="py-1">
              <span className="text-zinc-400 text-sm font-mono">{agentPath}</span>
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}
