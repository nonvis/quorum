#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <iostream>

#include <sqlite3.h>

#include "utils/uuid.h"

namespace sui::quorum {

// Phase 14.1c (FIX A) — a knower VAULT_UPDATE held behind the brainstorm gate.
// Staged when the gate suppresses the write; flushed to the knower's own vault
// once a human approves (gate_cleared). Mirrors the VaultUpdate payload plus the
// emitting agent's id/role/mode so the flush can route it to the right vault.
struct PendingVaultUpdate {
    std::string agent_id;
    std::string role;
    std::string mode;
    std::string path;
    std::string content;
};

struct ConversationRecord {
    int64_t id{0};
    std::string goal;
    std::string state;
    int round{0};
    int max_rounds{3};
    double budget_usd{5.0};
    double spent_usd{0.0};
    std::string current_agent;  // who has the ball
    int path_index{0};          // position in default_path
    std::string team;
    std::string mode{"generic"};  // execution mode: "generic" (default) or "brainstorm"
    bool no_vault_write{false};   // Phase 10 Track 5: suppress VAULT_UPDATE filesystem writes
    bool gated{false};            // Phase 14.1: gated brainstorm — knower writes wait for approval
    bool gate_cleared{false};     // Phase 14.1: human has approved (respond() flipped it)
};

class Database {
public:
    explicit Database(const std::string& path) {
        int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
        int rc = sqlite3_open_v2(path.c_str(), &db_, flags, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "ERROR: sqlite3_open: " << sqlite3_errmsg(db_) << "\n";
            db_ = nullptr;
            return;
        }
        exec_raw("PRAGMA journal_mode=WAL");
        exec_raw("PRAGMA synchronous=NORMAL");
        exec_raw("PRAGMA foreign_keys=ON");
        exec_raw("PRAGMA busy_timeout=5000");
    }

    ~Database() {
        if (db_) sqlite3_close(db_);
    }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    [[nodiscard]] bool is_open() const { return db_ != nullptr; }

    // Returns last insert rowid (useful after INSERT)
    [[nodiscard]] int64_t last_insert_id() {
        std::lock_guard<std::mutex> lock(mutex_);
        return sqlite3_last_insert_rowid(db_);
    }

    // ── Conversation CRUD ──────────────────────────────────────────────────

    int64_t create_conversation(const std::string& goal, int max_rounds) {
        // budget_usd column omitted — takes its schema DEFAULT (5.0).
        execute(
            "INSERT INTO conversations (goal, state, round, max_rounds) "
            "VALUES (?, 'active', 0, ?)",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, goal.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 2, max_rounds);
            }
        );
        return last_insert_id();
    }

    void update_conversation_state(int64_t conv_id, const std::string& state) {
        execute(
            "UPDATE conversations SET state = ? WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, state.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 2, conv_id);
            }
        );
    }

    void update_conversation_round(int64_t conv_id, int round) {
        execute(
            "UPDATE conversations SET round = ? WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int(stmt, 1, round);
                sqlite3_bind_int64(stmt, 2, conv_id);
            }
        );
    }

    void update_conversation_spent(int64_t conv_id, double additional_cost) {
        execute(
            "UPDATE conversations SET spent_usd = spent_usd + ? WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_double(stmt, 1, additional_cost);
                sqlite3_bind_int64(stmt, 2, conv_id);
            }
        );
    }

    void pause_conversation(int64_t conv_id, const std::string& reason) {
        execute(
            "UPDATE conversations SET state = 'paused', paused_reason = ? WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, reason.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 2, conv_id);
            }
        );
    }

    // Phase 14.1 — flip the brainstorm gate to cleared (a human has approved).
    // Called from respond(); once set, the daemon stops suppressing knower
    // VAULT_UPDATE writes for this gated brainstorm.
    void set_gate_cleared(int64_t conv_id, bool cleared) {
        execute(
            "UPDATE conversations SET gate_cleared = ? WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int(stmt, 1, cleared ? 1 : 0);
                sqlite3_bind_int64(stmt, 2, conv_id);
            }
        );
    }

    void complete_conversation(int64_t conv_id) {
        execute(
            "UPDATE conversations SET state = 'done', completed_at = datetime('now') WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conv_id);
            }
        );
    }

    [[nodiscard]] std::optional<ConversationRecord> get_conversation(int64_t conv_id) {
        ConversationRecord rec;
        bool found = false;
        query(
            "SELECT id, goal, state, round, max_rounds, budget_usd, spent_usd, "
            "current_agent, path_index, team, COALESCE(mode, 'generic'), "
            "COALESCE(no_vault_write, 0), COALESCE(gated, 0), "
            "COALESCE(gate_cleared, 0) "
            "FROM conversations WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conv_id);
            },
            [&](sqlite3_stmt* stmt) {
                found = true;
                rec.id = sqlite3_column_int64(stmt, 0);
                auto g = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                rec.goal = g ? g : "";
                auto s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                rec.state = s ? s : "";
                rec.round = sqlite3_column_int(stmt, 3);
                rec.max_rounds = sqlite3_column_int(stmt, 4);
                rec.budget_usd = sqlite3_column_double(stmt, 5);
                rec.spent_usd = sqlite3_column_double(stmt, 6);
                auto ca = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
                rec.current_agent = ca ? ca : "";
                rec.path_index = sqlite3_column_int(stmt, 8);
                auto tm = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
                rec.team = tm ? tm : "";
                auto md = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
                rec.mode = md ? md : "generic";
                rec.no_vault_write = sqlite3_column_int(stmt, 11) != 0;
                rec.gated = sqlite3_column_int(stmt, 12) != 0;
                rec.gate_cleared = sqlite3_column_int(stmt, 13) != 0;
            }
        );
        return found ? std::optional{rec} : std::nullopt;
    }

    [[nodiscard]] std::optional<int64_t> get_conversation_for_task(int64_t task_id) {
        int64_t conv_id = 0;
        bool found = false;
        query(
            "SELECT conversation_id FROM tasks WHERE id = ? AND conversation_id IS NOT NULL",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, task_id);
            },
            [&](sqlite3_stmt* stmt) {
                if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
                    found = true;
                    conv_id = sqlite3_column_int64(stmt, 0);
                }
            }
        );
        return found ? std::optional{conv_id} : std::nullopt;
    }

    // Execute with return value indicating success
    [[nodiscard]] bool execute_ok(const std::string& sql) {
        std::lock_guard<std::mutex> lock(mutex_);
        char* err = nullptr;
        int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            std::cerr << "ERROR: exec: " << (err ? err : "unknown") << "\n";
            if (err) sqlite3_free(err);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool execute_ok(const std::string& sql, std::function<void(sqlite3_stmt*)> bind) {
        std::lock_guard<std::mutex> lock(mutex_);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "ERROR: prepare: " << sqlite3_errmsg(db_) << "\n";
            return false;
        }
        bind(stmt);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }

    // Query that returns a single int64 value (for aggregates like SUM, COUNT)
    [[nodiscard]] int64_t query_int(const std::string& sql, int64_t fallback = 0) {
        int64_t result = fallback;
        query(sql, [&](sqlite3_stmt* stmt) {
            if (sqlite3_column_type(stmt, 0) != SQLITE_NULL)
                result = sqlite3_column_int64(stmt, 0);
        });
        return result;
    }

    [[nodiscard]] double query_double(const std::string& sql, double fallback = 0.0) {
        double result = fallback;
        query(sql, [&](sqlite3_stmt* stmt) {
            if (sqlite3_column_type(stmt, 0) != SQLITE_NULL)
                result = sqlite3_column_double(stmt, 0);
        });
        return result;
    }

    void execute(const std::string& sql) {
        std::lock_guard<std::mutex> lock(mutex_);
        exec_raw(sql);
    }

    void execute(const std::string& sql, std::function<void(sqlite3_stmt*)> bind) {
        std::lock_guard<std::mutex> lock(mutex_);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "ERROR: prepare: " << sqlite3_errmsg(db_) << "\n";
            return;
        }
        bind(stmt);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    void query(const std::string& sql,
               std::function<void(sqlite3_stmt*)> bind,
               std::function<void(sqlite3_stmt*)> row_callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "ERROR: prepare: " << sqlite3_errmsg(db_) << "\n";
            return;
        }
        bind(stmt);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            row_callback(stmt);
        }
        sqlite3_finalize(stmt);
    }

    void query(const std::string& sql,
               std::function<void(sqlite3_stmt*)> row_callback) {
        query(sql, [](sqlite3_stmt*){}, row_callback);
    }

    class Transaction {
    public:
        explicit Transaction(Database& db) : db_(db) {
            db_.execute("BEGIN TRANSACTION");
        }
        ~Transaction() {
            if (!committed_) db_.execute("ROLLBACK");
        }
        void commit() {
            db_.execute("COMMIT");
            committed_ = true;
        }
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;
    private:
        Database& db_;
        bool committed_ = false;
    };

    // ── Agent Sessions ──────────────────────────────────────────────────

    std::string get_or_create_session(int64_t cycle_id, const std::string& agent_id) {
        std::string session_id;
        query(
            "SELECT session_id FROM agent_sessions WHERE cycle_id = ? AND agent_id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, cycle_id);
                sqlite3_bind_text(stmt, 2, agent_id.c_str(), -1, SQLITE_TRANSIENT);
            },
            [&](sqlite3_stmt* stmt) {
                auto s = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (s) session_id = s;
            }
        );
        if (!session_id.empty()) return session_id;

        session_id = generate_uuid();
        execute(
            "INSERT INTO agent_sessions (cycle_id, agent_id, session_id) VALUES (?, ?, ?)",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, cycle_id);
                sqlite3_bind_text(stmt, 2, agent_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, session_id.c_str(), -1, SQLITE_TRANSIENT);
            }
        );
        return session_id;
    }

    sqlite3* handle() { return db_; }

    // ── Evaluations (Phase 8 Track 3) ─────────────────────────────────────

    // Insert an evaluation row. Returns the new row id.
    // score_json is the raw per-item breakdown as a JSON string (stored as-is).
    int64_t append_evaluation(int64_t conversation_id,
                              const std::string& scored_agent_id,
                              const std::string& evaluator_agent_id,
                              const std::string& role_specialty,
                              const std::string& rubric_version,
                              double score_total,
                              const std::string& score_json,
                              const std::string& notes) {
        execute(
            "INSERT INTO evaluations "
            "(conversation_id, scored_agent_id, evaluator_agent_id, "
            " role_specialty, rubric_version, score_total, score_json, notes) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conversation_id);
                sqlite3_bind_text(stmt, 2, scored_agent_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, evaluator_agent_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, role_specialty.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 5, rubric_version.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(stmt, 6, score_total);
                sqlite3_bind_text(stmt, 7, score_json.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 8, notes.c_str(), -1, SQLITE_TRANSIENT);
            }
        );
        return last_insert_id();
    }

    // Most recent task agent in a conversation other than `excluded_agent`.
    // Used to deduce scored_agent_id when an EVALUATION block omits `scored:`.
    // Returns empty string if no such task exists.
    std::string previous_task_agent(int64_t conversation_id,
                                    const std::string& excluded_agent) {
        std::string agent;
        query(
            "SELECT agent FROM tasks "
            "WHERE conversation_id = ? AND agent != ? "
            "ORDER BY id DESC LIMIT 1",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conversation_id);
                sqlite3_bind_text(stmt, 2, excluded_agent.c_str(), -1, SQLITE_TRANSIENT);
            },
            [&](sqlite3_stmt* stmt) {
                auto a = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (a) agent = a;
            }
        );
        return agent;
    }

    // ── Pending vault updates (Phase 14.1c, FIX A) ────────────────────────
    // Knower VAULT_UPDATEs produced before the human approves a gated
    // brainstorm are STAGED here (instead of dropped) and FLUSHED to their
    // vaults once the gate clears.

    // Stage one held VAULT_UPDATE for later flush.
    void stage_vault_update(int64_t conv_id, const std::string& agent_id,
                            const std::string& role, const std::string& mode,
                            const std::string& path, const std::string& content) {
        execute(
            "INSERT INTO pending_vault_updates "
            "(conversation_id, agent_id, role, mode, path, content) "
            "VALUES (?, ?, ?, ?, ?, ?)",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conv_id);
                sqlite3_bind_text(stmt, 2, agent_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, role.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, mode.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 5, path.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 6, content.c_str(), -1, SQLITE_TRANSIENT);
            }
        );
    }

    // All staged updates for a conversation, oldest first (insertion order).
    [[nodiscard]] std::vector<PendingVaultUpdate> get_pending_vault_updates(int64_t conv_id) {
        std::vector<PendingVaultUpdate> out;
        query(
            "SELECT agent_id, role, mode, path, content "
            "FROM pending_vault_updates WHERE conversation_id = ? ORDER BY id",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conv_id);
            },
            [&](sqlite3_stmt* stmt) {
                PendingVaultUpdate pu;
                auto a = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                pu.agent_id = a ? a : "";
                auto r = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                pu.role = r ? r : "";
                auto m = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                pu.mode = m ? m : "";
                auto p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                pu.path = p ? p : "";
                auto c = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
                pu.content = c ? c : "";
                out.push_back(std::move(pu));
            }
        );
        return out;
    }

    // Drop all staged updates for a conversation (after a successful flush).
    void clear_pending_vault_updates(int64_t conv_id) {
        execute(
            "DELETE FROM pending_vault_updates WHERE conversation_id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conv_id);
            }
        );
    }

    // How many updates are currently staged for a conversation.
    [[nodiscard]] int count_pending_vault_updates(int64_t conv_id) {
        int count = 0;
        query(
            "SELECT COUNT(*) FROM pending_vault_updates WHERE conversation_id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int64(stmt, 1, conv_id);
            },
            [&](sqlite3_stmt* stmt) {
                count = sqlite3_column_int(stmt, 0);
            }
        );
        return count;
    }

private:
    sqlite3* db_ = nullptr;
    std::mutex mutex_;

    void exec_raw(const std::string& sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::cerr << "ERROR: exec: " << (err ? err : "unknown") << "\n";
            if (err) sqlite3_free(err);
        }
    }
};

} // namespace sui::quorum
