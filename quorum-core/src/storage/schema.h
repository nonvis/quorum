#pragma once

#include "storage/database.h"

namespace sui::quorum {

// Create all tables and indexes. Safe to call on existing DB (uses IF NOT EXISTS).
// Does NOT include ALTER TABLE migrations — those live in main.cpp init_schema().
inline void create_schema(Database& db) {
    db.execute(
        "CREATE TABLE IF NOT EXISTS audit_log ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp INTEGER NOT NULL,"
        "  agent TEXT NOT NULL,"
        "  action TEXT NOT NULL,"
        "  details TEXT"
        ")"
    );
    db.execute(
        "CREATE TABLE IF NOT EXISTS conversations ("
        "  id INTEGER PRIMARY KEY,"
        "  goal TEXT NOT NULL,"
        "  state TEXT NOT NULL DEFAULT 'active',"
        "  round INTEGER NOT NULL DEFAULT 0,"
        "  max_rounds INTEGER NOT NULL DEFAULT 3,"
        "  budget_usd REAL NOT NULL DEFAULT 5.0,"
        "  spent_usd REAL NOT NULL DEFAULT 0.0,"
        "  created_at TEXT NOT NULL DEFAULT (datetime('now')),"
        "  completed_at TEXT,"
        "  paused_reason TEXT,"
        "  current_agent TEXT,"
        "  path_index INTEGER NOT NULL DEFAULT 0,"
        "  mode TEXT NOT NULL DEFAULT 'generic',"
        "  no_vault_write INTEGER NOT NULL DEFAULT 0,"
        // Phase 14.1 — daemon-enforced brainstorm gate. `gated` marks a
        // brainstorm whose knower VAULT_UPDATE writes are suppressed until a
        // human approves; `gate_cleared` flips to 1 once a human responds to
        // the waiting_for_human gate (see respond()).
        "  gated INTEGER NOT NULL DEFAULT 0,"
        "  gate_cleared INTEGER NOT NULL DEFAULT 0"
        ")"
    );
    db.execute(
        "CREATE TABLE IF NOT EXISTS tasks ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  agent TEXT NOT NULL,"
        "  task_type TEXT NOT NULL,"
        "  status TEXT NOT NULL DEFAULT 'pending',"
        "  prompt TEXT NOT NULL,"
        "  result TEXT,"
        "  token_in INTEGER,"
        "  token_out INTEGER,"
        "  cost REAL,"
        "  error TEXT,"
        "  created_at TEXT NOT NULL DEFAULT (datetime('now')),"
        "  started_at TEXT,"
        "  completed_at TEXT,"
        "  conversation_id INTEGER REFERENCES conversations(id),"
        "  session_id TEXT,"
        "  system_prompt TEXT"  // Phase 7 Track 5 — stable per-agent prefix
        ")"
    );
    db.execute("CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status)");
    db.execute("CREATE INDEX IF NOT EXISTS idx_tasks_agent ON tasks(agent)");
    db.execute(
        "CREATE TABLE IF NOT EXISTS agent_sessions ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  cycle_id    INTEGER NOT NULL REFERENCES conversations(id),"
        "  agent_id    TEXT NOT NULL,"
        "  session_id  TEXT NOT NULL,"
        "  UNIQUE(cycle_id, agent_id)"
        ")"
    );
    db.execute(
        "CREATE TABLE IF NOT EXISTS budget_window ("
        "  id INTEGER PRIMARY KEY CHECK (id = 1),"
        "  budget_usd REAL NOT NULL DEFAULT 100.0,"
        "  window_hours REAL NOT NULL DEFAULT 5.0,"
        "  window_start TEXT NOT NULL DEFAULT (datetime('now')),"
        "  spent_usd REAL NOT NULL DEFAULT 0.0"
        ")"
    );

    // Phase 8 Track 3 — evaluator scores from EVALUATION blocks
    db.execute(
        "CREATE TABLE IF NOT EXISTS evaluations ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  conversation_id INTEGER NOT NULL REFERENCES conversations(id),"
        "  scored_agent_id TEXT NOT NULL,"
        "  evaluator_agent_id TEXT NOT NULL,"
        "  role_specialty TEXT NOT NULL,"
        "  rubric_version TEXT NOT NULL,"
        "  score_total REAL NOT NULL,"            // normalized 0-100
        "  score_json TEXT NOT NULL,"             // per-item breakdown (JSON array)
        "  notes TEXT,"                           // evaluator's free-form summary
        "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
        ")"
    );
    db.execute("CREATE INDEX IF NOT EXISTS idx_evaluations_conv "
               "ON evaluations(conversation_id)");
    db.execute("CREATE INDEX IF NOT EXISTS idx_evaluations_scored "
               "ON evaluations(scored_agent_id)");

    // Phase 14.1c (FIX A) — knower VAULT_UPDATEs held behind the brainstorm
    // gate. Staged on suppression, flushed to the knower's vault once a human
    // approves (gate_cleared). See Database::stage/get/clear/count_pending_*.
    db.execute(
        "CREATE TABLE IF NOT EXISTS pending_vault_updates ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  conversation_id INTEGER NOT NULL,"
        "  agent_id TEXT NOT NULL,"
        "  role TEXT NOT NULL,"
        "  mode TEXT NOT NULL,"
        "  path TEXT NOT NULL,"
        "  content TEXT NOT NULL,"
        "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
        ")"
    );
    db.execute("CREATE INDEX IF NOT EXISTS idx_pending_vault_updates_conv "
               "ON pending_vault_updates(conversation_id)");
}

} // namespace sui::quorum
