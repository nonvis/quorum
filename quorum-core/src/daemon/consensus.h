#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "storage/database.h"
#include "utils/config.h"

namespace sui::quorum {

enum class ProposalState : uint8_t {
    Draft      = 0,
    Reviewing  = 1,
    Approved   = 2,
    Rejected   = 3,
    Escalated  = 4,
    Executed   = 5,
    Evaluated  = 6,
};

struct LocalProposal {
    std::string id;
    std::string title;
    std::string author;
    std::string content;
    ProposalState state{ProposalState::Draft};
    uint64_t created_at{0};
    uint64_t updated_at{0};
    uint8_t current_round{0};
    std::vector<std::string> required_reviewers;
    std::vector<std::string> approvals;
    std::vector<std::string> rejections;
    int64_t source_task_id{0};
    uint64_t max_rounds{3};
};

class ConsensusEngine {
public:
    ConsensusEngine(Database& db, const ConsensusConfig& cfg)
        : db_(db), cfg_(cfg) {
        std::srand(static_cast<unsigned>(std::time(nullptr)));
    }

    // ── Schema ────────────────────────────────────────────────────────────────

    static void init_schema(Database& db) {
        db.execute(
            "CREATE TABLE IF NOT EXISTS proposals ("
            "  id TEXT PRIMARY KEY,"
            "  title TEXT NOT NULL,"
            "  author TEXT NOT NULL,"
            "  content TEXT NOT NULL,"
            "  state INTEGER NOT NULL DEFAULT 0,"
            "  required_reviewers TEXT NOT NULL,"
            "  created_at INTEGER NOT NULL,"
            "  updated_at INTEGER NOT NULL,"
            "  current_round INTEGER NOT NULL DEFAULT 0,"
            "  source_task_id INTEGER,"
            "  max_rounds INTEGER NOT NULL DEFAULT 3"
            ")"
        );
        db.execute("CREATE INDEX IF NOT EXISTS idx_proposals_state ON proposals(state)");
        db.execute("CREATE INDEX IF NOT EXISTS idx_proposals_author ON proposals(author)");

        db.execute(
            "CREATE TABLE IF NOT EXISTS reviews ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  proposal_id TEXT NOT NULL,"
            "  reviewer TEXT NOT NULL,"
            "  round INTEGER NOT NULL,"
            "  verdict TEXT NOT NULL,"
            "  reasoning TEXT,"
            "  created_at INTEGER NOT NULL,"
            "  source_task_id INTEGER,"
            "  UNIQUE(proposal_id, reviewer, round)"
            ")"
        );
        db.execute("CREATE INDEX IF NOT EXISTS idx_reviews_proposal ON reviews(proposal_id)");
    }

    // ── Public API ────────────────────────────────────────────────────────────

    /// Create a proposal and immediately transition it to REVIEWING.
    /// Returns the generated proposal ID.
    std::string create_proposal(const std::string& author,
                                const std::string& title,
                                const std::string& content,
                                const std::vector<std::string>& required_reviewers,
                                int64_t source_task_id = 0) {
        auto now = epoch_seconds();
        auto id = generate_id(now);
        auto reviewers_csv = join_csv(required_reviewers);

        // Insert directly as REVIEWING (state=1) with current_round=1.
        // Conceptually: DRAFT -> REVIEWING is immediate when required_reviewers
        // are known at creation time, so we skip the transient DRAFT state.
        db_.execute(
            "INSERT INTO proposals (id, title, author, content, state, "
            "required_reviewers, created_at, updated_at, current_round, "
            "source_task_id, max_rounds) "
            "VALUES (?, ?, ?, ?, 1, ?, ?, ?, 1, ?, ?)",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, title.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, author.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, content.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 5, reviewers_csv.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 6, static_cast<int64_t>(now));
                sqlite3_bind_int64(stmt, 7, static_cast<int64_t>(now));
                sqlite3_bind_int64(stmt, 8, source_task_id);
                sqlite3_bind_int64(stmt, 9, static_cast<int64_t>(cfg_.max_rounds));
            }
        );

        return id;
    }

    /// Submit a review for a proposal currently in REVIEWING state.
    /// Validates reviewer eligibility and triggers round evaluation.
    bool submit_review(const std::string& proposal_id,
                       const std::string& reviewer,
                       const std::string& verdict,
                       const std::string& reasoning,
                       int64_t source_task_id = 0) {
        auto prop = get_proposal(proposal_id);
        if (!prop) return false;
        if (prop->state != ProposalState::Reviewing) return false;

        // Reviewer must be in required_reviewers
        bool is_required = false;
        for (const auto& r : prop->required_reviewers) {
            if (r == reviewer) { is_required = true; break; }
        }
        if (!is_required) return false;

        // Proactive duplicate check (UNIQUE constraint is the backstop)
        int64_t existing = 0;
        db_.query(
            "SELECT COUNT(*) FROM reviews "
            "WHERE proposal_id = ? AND reviewer = ? AND round = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, proposal_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, reviewer.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 3, prop->current_round);
            },
            [&](sqlite3_stmt* stmt) {
                existing = sqlite3_column_int64(stmt, 0);
            }
        );
        if (existing > 0) return false;

        auto now = epoch_seconds();
        bool ok = db_.execute_ok(
            "INSERT INTO reviews (proposal_id, reviewer, round, verdict, "
            "reasoning, created_at, source_task_id) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, proposal_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, reviewer.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 3, prop->current_round);
                sqlite3_bind_text(stmt, 4, verdict.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 5, reasoning.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 6, static_cast<int64_t>(now));
                sqlite3_bind_int64(stmt, 7, source_task_id);
            }
        );
        if (!ok) return false;

        evaluate_round(proposal_id);
        return true;
    }

    /// Retrieve a single proposal by ID, including current-round review data.
    [[nodiscard]] std::optional<LocalProposal> get_proposal(const std::string& proposal_id) {
        LocalProposal prop;
        bool found = false;

        db_.query(
            "SELECT id, title, author, content, state, required_reviewers, "
            "created_at, updated_at, current_round, source_task_id, max_rounds "
            "FROM proposals WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, proposal_id.c_str(), -1, SQLITE_TRANSIENT);
            },
            [&](sqlite3_stmt* stmt) {
                found = true;
                prop.id = col_text(stmt, 0);
                prop.title = col_text(stmt, 1);
                prop.author = col_text(stmt, 2);
                prop.content = col_text(stmt, 3);
                prop.state = static_cast<ProposalState>(sqlite3_column_int(stmt, 4));
                prop.required_reviewers = split_csv(col_text(stmt, 5));
                prop.created_at = static_cast<uint64_t>(sqlite3_column_int64(stmt, 6));
                prop.updated_at = static_cast<uint64_t>(sqlite3_column_int64(stmt, 7));
                prop.current_round = static_cast<uint8_t>(sqlite3_column_int(stmt, 8));
                prop.source_task_id = sqlite3_column_int64(stmt, 9);
                prop.max_rounds = static_cast<uint64_t>(sqlite3_column_int64(stmt, 10));
            }
        );

        if (!found) return std::nullopt;

        // Populate approvals/rejections for the current round
        db_.query(
            "SELECT reviewer, verdict FROM reviews "
            "WHERE proposal_id = ? AND round = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, proposal_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 2, prop.current_round);
            },
            [&](sqlite3_stmt* stmt) {
                auto reviewer = col_text(stmt, 0);
                auto verdict = col_text(stmt, 1);
                if (verdict == "approve") {
                    prop.approvals.push_back(reviewer);
                } else if (verdict == "reject" || verdict == "escalate") {
                    prop.rejections.push_back(reviewer);
                }
            }
        );

        return prop;
    }

    /// Retrieve all proposals in a given state.
    [[nodiscard]] std::vector<LocalProposal> get_proposals_by_state(ProposalState state) {
        std::vector<std::string> ids;

        db_.query(
            "SELECT id FROM proposals WHERE state = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int(stmt, 1, static_cast<int>(state));
            },
            [&](sqlite3_stmt* stmt) {
                ids.push_back(col_text(stmt, 0));
            }
        );

        std::vector<LocalProposal> results;
        for (const auto& id : ids) {
            auto prop = get_proposal(id);
            if (prop) results.push_back(std::move(*prop));
        }
        return results;
    }

    /// Returns required_reviewers who haven't yet reviewed in the current round.
    /// The daemon uses this to schedule review tasks.
    [[nodiscard]] std::vector<std::string> get_pending_reviewers(const std::string& proposal_id) {
        auto prop = get_proposal(proposal_id);
        if (!prop || prop->state != ProposalState::Reviewing) return {};

        std::vector<std::string> reviewed;
        db_.query(
            "SELECT reviewer FROM reviews "
            "WHERE proposal_id = ? AND round = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, proposal_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 2, prop->current_round);
            },
            [&](sqlite3_stmt* stmt) {
                reviewed.push_back(col_text(stmt, 0));
            }
        );

        std::vector<std::string> pending;
        for (const auto& r : prop->required_reviewers) {
            bool done = false;
            for (const auto& d : reviewed) {
                if (d == r) { done = true; break; }
            }
            if (!done) pending.push_back(r);
        }
        return pending;
    }

    /// Count proposals grouped by state (for heartbeat/stats logging).
    [[nodiscard]] std::map<ProposalState, int64_t> proposal_count_by_state() {
        std::map<ProposalState, int64_t> counts;
        db_.query(
            "SELECT state, COUNT(*) FROM proposals GROUP BY state",
            [&](sqlite3_stmt* stmt) {
                auto state = static_cast<ProposalState>(sqlite3_column_int(stmt, 0));
                auto count = sqlite3_column_int64(stmt, 1);
                counts[state] = count;
            }
        );
        return counts;
    }

    /// Validate whether a state transition is legal per the proposal state machine.
    [[nodiscard]] bool can_transition(ProposalState from, ProposalState to) const {
        switch (from) {
            case ProposalState::Draft:     return to == ProposalState::Reviewing;
            case ProposalState::Reviewing:
                return to == ProposalState::Approved ||
                       to == ProposalState::Rejected ||
                       to == ProposalState::Escalated;
            case ProposalState::Approved:  return to == ProposalState::Executed;
            case ProposalState::Executed:  return to == ProposalState::Evaluated;
            default: return false;
        }
    }

private:
    Database& db_;
    ConsensusConfig cfg_;

    // ── Helpers ───────────────────────────────────────────────────────────────

    static uint64_t epoch_seconds() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count());
    }

    std::string generate_id(uint64_t now) {
        int suffix = std::rand() % 10000;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "prop-%llu-%04d",
                      static_cast<unsigned long long>(now), suffix);
        return std::string(buf);
    }

    static std::string col_text(sqlite3_stmt* stmt, int col) {
        auto ptr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
        return ptr ? std::string(ptr) : std::string{};
    }

    static std::string join_csv(const std::vector<std::string>& items) {
        std::string result;
        for (size_t i = 0; i < items.size(); ++i) {
            if (i > 0) result += ',';
            result += items[i];
        }
        return result;
    }

    static std::vector<std::string> split_csv(const std::string& csv) {
        std::vector<std::string> items;
        if (csv.empty()) return items;
        std::istringstream ss(csv);
        std::string item;
        while (std::getline(ss, item, ',')) {
            size_t start = item.find_first_not_of(' ');
            if (start == std::string::npos) continue;
            size_t end = item.find_last_not_of(' ');
            items.push_back(item.substr(start, end - start + 1));
        }
        return items;
    }

    // ── State machine ─────────────────────────────────────────────────────────

    /// Transition a proposal to a new state in the DB, with validation.
    bool transition(const std::string& proposal_id, ProposalState new_state) {
        int current = -1;
        db_.query(
            "SELECT state FROM proposals WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, proposal_id.c_str(), -1, SQLITE_TRANSIENT);
            },
            [&](sqlite3_stmt* stmt) {
                current = sqlite3_column_int(stmt, 0);
            }
        );

        if (current < 0) return false;

        auto from = static_cast<ProposalState>(current);
        if (!can_transition(from, new_state)) return false;

        auto now = epoch_seconds();
        db_.execute(
            "UPDATE proposals SET state = ?, updated_at = ? WHERE id = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_int(stmt, 1, static_cast<int>(new_state));
                sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(now));
                sqlite3_bind_text(stmt, 3, proposal_id.c_str(), -1, SQLITE_TRANSIENT);
            }
        );
        return true;
    }

    /// Evaluate the current round after a review submission.
    /// If all reviewers have submitted:
    ///   - All approve -> APPROVED
    ///   - Any escalate -> ESCALATED
    ///   - Any reject + rounds remaining -> increment round (stay REVIEWING)
    ///   - Any reject + final round -> ESCALATED
    void evaluate_round(const std::string& proposal_id) {
        auto prop = get_proposal(proposal_id);
        if (!prop || prop->state != ProposalState::Reviewing) return;

        // Collect all reviews for the current round
        struct ReviewEntry {
            std::string reviewer;
            std::string verdict;
        };
        std::vector<ReviewEntry> round_reviews;

        db_.query(
            "SELECT reviewer, verdict FROM reviews "
            "WHERE proposal_id = ? AND round = ?",
            [&](sqlite3_stmt* stmt) {
                sqlite3_bind_text(stmt, 1, proposal_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 2, prop->current_round);
            },
            [&](sqlite3_stmt* stmt) {
                round_reviews.push_back({col_text(stmt, 0), col_text(stmt, 1)});
            }
        );

        // Wait until all required reviewers have submitted
        if (round_reviews.size() < prop->required_reviewers.size()) return;

        // Tally verdicts
        bool has_escalate = false;
        bool has_reject = false;
        for (const auto& r : round_reviews) {
            if (r.verdict == "escalate") has_escalate = true;
            if (r.verdict == "reject") has_reject = true;
        }

        if (!has_escalate && !has_reject) {
            // All approved
            transition(proposal_id, ProposalState::Approved);
        } else if (has_escalate) {
            // Any explicit escalate -> ESCALATED immediately
            transition(proposal_id, ProposalState::Escalated);
        } else {
            // Rejected — check if more rounds available
            if (static_cast<uint64_t>(prop->current_round) >= prop->max_rounds) {
                // Final round exhausted -> ESCALATED (human decides)
                transition(proposal_id, ProposalState::Escalated);
            } else {
                // Increment round, stay REVIEWING for another attempt
                auto now = epoch_seconds();
                db_.execute(
                    "UPDATE proposals SET current_round = current_round + 1, "
                    "updated_at = ? WHERE id = ?",
                    [&](sqlite3_stmt* stmt) {
                        sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(now));
                        sqlite3_bind_text(stmt, 2, proposal_id.c_str(), -1, SQLITE_TRANSIENT);
                    }
                );
            }
        }
    }
};

} // namespace sui::quorum
