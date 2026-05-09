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
        "  path_index INTEGER NOT NULL DEFAULT 0"
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
        "  session_id TEXT"
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
}

} // namespace sui::quorum
