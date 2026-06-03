// tests/unit/test_brainstorm_gate.cpp
// Phase 14.1 — daemon-enforced brainstorm gate + leader re-entry routing.
//
// The live run of Phase 14 Gate B proved two structural failures: (1) the human
// gate never fired because a knower wrote its VAULT_UPDATE before any approval
// (convention/SKILL text is insufficient — the invariant must be daemon
// enforced); and (2) the multi-lens brainstorm collapsed to one lens because a
// non-leader knower had no path back to the leader once it produced its turn.
//
// This test pins the daemon-side fixes:
//   A. `gated` defaults — brainstorm gates by default (1); --ungated forces 0;
//      generic is never gated (0).
//   B. respond() flips gate_cleared = 1 (the human-approval signal).
//   C. The pure invariant brainstorm_gate_suppresses_write(): a write is
//      SUPPRESSED iff brainstorm + gated + !gate_cleared; applied once cleared;
//      NEVER suppressed for ungated scans or generic mode.
//   D. Brainstorm re-entry routing — a NON-leader knower that emits NO HANDOFF
//      (or a HANDOFF to the leader) routes the ball back to the leader, NOT to
//      `done`. The leader is the only one that ends (to: done) or gates
//      (to: human).
//   E. An ungated single-knower scan (run-knower.sh shape: emit …, HANDOFF
//      done) still terminates — its explicit `to: done` is honored.
//
// Run:  cd build && ctest -R test_brainstorm_gate --output-on-failure

#include <cstdlib>
#include <iostream>
#include <string>

#include <sqlite3.h>

#include "storage/database.h"
#include "daemon/conversation.h"
#include "agent/output_parser.h"
#include "utils/config.h"

static int g_passed = 0;
static int g_failed = 0;

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        ++g_failed;
        std::exit(1);
    }
    std::cout << "[PASS] " << msg << "\n";
    ++g_passed;
}

// Schema mirrors the production conversations/tasks/agent_sessions tables,
// INCLUDING the Phase 14.1 gated/gate_cleared columns (get_conversation() reads
// them).
static void init_schema(sui::quorum::Database& db) {
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
        "  team TEXT,"
        "  mode TEXT NOT NULL DEFAULT 'generic',"
        "  no_vault_write INTEGER NOT NULL DEFAULT 0,"
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
        "  system_prompt TEXT,"
        "  cache_creation_input_tokens INTEGER,"
        "  cache_read_input_tokens INTEGER"
        ")"
    );
    db.execute(
        "CREATE TABLE IF NOT EXISTS agent_sessions ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  cycle_id    INTEGER NOT NULL REFERENCES conversations(id),"
        "  agent_id    TEXT NOT NULL,"
        "  session_id  TEXT NOT NULL,"
        "  UNIQUE(cycle_id, agent_id)"
        ")"
    );
    // Phase 14.1c (FIX A) — staged knower writes held behind the gate.
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
}

struct TestHarness {
    sui::quorum::Database db;
    sui::quorum::ConversationConfig cfg;
    std::vector<sui::quorum::AgentMetadata> agents;

    TestHarness() : db(":memory:") {
        init_schema(db);
        cfg.leader = "leader";
        cfg.default_max_rounds = 20;
        cfg.default_budget_usd = 5.0;
        // No default_path — mirrors the post-team-removal roster where routing
        // is HANDOFF-driven and a no-HANDOFF should fall to the leader (in
        // brainstorm) rather than to `done`.
        agents.push_back(sui::quorum::AgentMetadata{.id = "leader",    .role = "leader"});
        agents.push_back(sui::quorum::AgentMetadata{.id = "architect", .role = "thinker"});
        agents.push_back(sui::quorum::AgentMetadata{.id = "historian", .role = "thinker"});
    }

    sui::quorum::ConversationEngine make_engine() {
        return sui::quorum::ConversationEngine(db, cfg, agents);
    }

    void complete_task(int64_t task_id) {
        db.execute(
            "UPDATE tasks SET status = 'done', completed_at = datetime('now') WHERE id = ?",
            [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, task_id); });
    }

    int64_t latest_pending_task(int64_t conv_id) {
        return db.query_int(
            "SELECT COALESCE(MAX(id), 0) FROM tasks WHERE conversation_id = " +
            std::to_string(conv_id) + " AND status = 'pending'");
    }

    std::string latest_pending_agent(int64_t conv_id) {
        std::string agent;
        db.query(
            "SELECT agent FROM tasks WHERE conversation_id = ? AND status = 'pending' "
            "ORDER BY id DESC LIMIT 1",
            [&](sqlite3_stmt* stmt) { sqlite3_bind_int64(stmt, 1, conv_id); },
            [&](sqlite3_stmt* stmt) {
                auto a = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (a) agent = a;
            });
        return agent;
    }
};

// ─── A. gated defaults ───────────────────────────────────────────────────────

static void test_gated_defaults() {
    std::cout << "\n=== A. gated defaults (brainstorm=1, --ungated=0, generic=0) ===\n\n";

    // A1: brainstorm (auto, gated=-1) → gated=1, gate_cleared=0
    {
        TestHarness h;
        auto engine = h.make_engine();
        auto conv_id = engine.start("Discuss design", 5.0, 20, "brainstorm");
        auto conv = h.db.get_conversation(conv_id);
        check(conv && conv->mode == "brainstorm", "A1: mode == brainstorm");
        check(conv && conv->gated, "A1: brainstorm gates by default (gated=1)");
        check(conv && !conv->gate_cleared, "A1: gate_cleared starts 0");
    }
    // A2: brainstorm + --ungated (gated=0) → gated=0
    {
        TestHarness h;
        auto engine = h.make_engine();
        auto conv_id = engine.start("Scan repo", 5.0, 20, "brainstorm",
                                    /*no_vault_write=*/false, /*gated=*/0);
        auto conv = h.db.get_conversation(conv_id);
        check(conv && conv->mode == "brainstorm", "A2: mode == brainstorm");
        check(conv && !conv->gated, "A2: --ungated forces gated=0 (single-knower scan)");
    }
    // A3: generic (auto) → gated=0
    {
        TestHarness h;
        auto engine = h.make_engine();
        auto conv_id = engine.start("Build X", 5.0, 20);  // generic
        auto conv = h.db.get_conversation(conv_id);
        check(conv && conv->mode == "generic", "A3: mode == generic");
        check(conv && !conv->gated, "A3: generic is never gated (gated=0)");
    }
}

// ─── B. respond() clears the gate ────────────────────────────────────────────

static void test_respond_clears_gate() {
    std::cout << "\n=== B. respond() sets gate_cleared = 1 ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();
    auto conv_id = engine.start("Discuss design", 5.0, 20, "brainstorm");

    // Leader turn → HANDOFF to: human (gate)
    auto t1 = h.latest_pending_task(conv_id);
    h.complete_task(t1);
    sui::quorum::ParsedOutput parsed;
    parsed.handoff = sui::quorum::HandoffBlock{.to = "human",
                                               .prompt = "approve writes?"};
    bool active = engine.on_task_complete(t1, parsed, 0.05);
    check(!active, "B: conversation parked at the gate (not active)");

    auto pre = h.db.get_conversation(conv_id);
    check(pre && pre->state == "waiting_for_human", "B: state == waiting_for_human");
    check(pre && !pre->gate_cleared, "B: gate_cleared still 0 before respond");

    bool ok = engine.respond(conv_id, "yes");
    check(ok, "B: respond() accepted");

    auto post = h.db.get_conversation(conv_id);
    check(post && post->gate_cleared, "B: respond() flipped gate_cleared = 1");
    check(post && post->state == "active", "B: conversation active again after respond");
}

// ─── C. the pure suppression invariant ───────────────────────────────────────

static void test_suppression_invariant() {
    std::cout << "\n=== C. brainstorm_gate_suppresses_write (the core invariant) ===\n\n";

    using sui::quorum::brainstorm_gate_suppresses_write;

    // Gated brainstorm, not cleared → SUPPRESS.
    check(brainstorm_gate_suppresses_write("brainstorm", /*gated=*/true,
                                           /*cleared=*/false),
          "C: gated brainstorm + !cleared → write SUPPRESSED");
    // Gated brainstorm, cleared → APPLY.
    check(!brainstorm_gate_suppresses_write("brainstorm", /*gated=*/true,
                                            /*cleared=*/true),
          "C: gated brainstorm + cleared → write APPLIED");
    // Ungated brainstorm (single-knower scan) → APPLY regardless of cleared.
    check(!brainstorm_gate_suppresses_write("brainstorm", /*gated=*/false,
                                            /*cleared=*/false),
          "C: ungated brainstorm → write APPLIED (single-knower scan)");
    // Generic mode → never suppressed by this gate.
    check(!brainstorm_gate_suppresses_write("generic", /*gated=*/true,
                                            /*cleared=*/false),
          "C: generic mode → write APPLIED (gate is brainstorm-only)");
    check(!brainstorm_gate_suppresses_write("generic", /*gated=*/false,
                                            /*cleared=*/false),
          "C: generic ungated → write APPLIED");
}

// ─── C2. end-to-end suppression then apply across the gate ───────────────────
//
// Walk a gated brainstorm conversation through the daemon's get_conversation()
// view that the apply site reads: before respond() the predicate suppresses;
// after respond() it applies. This pins the *conversation-state* wiring (start
// → gate → respond) to the predicate, not just the predicate in isolation.

static void test_suppression_across_gate_e2e() {
    std::cout << "\n=== C2. suppression holds pre-approval, releases post-approval ===\n\n";

    using sui::quorum::brainstorm_gate_suppresses_write;

    TestHarness h;
    auto engine = h.make_engine();
    auto conv_id = engine.start("Discuss design", 5.0, 20, "brainstorm");

    // Pre-approval: a knower "write now" would be suppressed.
    {
        auto c = h.db.get_conversation(conv_id);
        check(c && brainstorm_gate_suppresses_write(c->mode, c->gated, c->gate_cleared),
              "C2: pre-approval — knower VAULT_UPDATE SUPPRESSED");
    }

    // Run to the gate, then approve.
    auto t1 = h.latest_pending_task(conv_id);
    h.complete_task(t1);
    sui::quorum::ParsedOutput gate;
    gate.handoff = sui::quorum::HandoffBlock{.to = "human", .prompt = "approve?"};
    engine.on_task_complete(t1, gate, 0.05);
    engine.respond(conv_id, "yes");

    // Post-approval: the same knower write now APPLIES.
    {
        auto c = h.db.get_conversation(conv_id);
        check(c && !brainstorm_gate_suppresses_write(c->mode, c->gated, c->gate_cleared),
              "C2: post-approval — knower VAULT_UPDATE APPLIED");
    }
}

// ─── D. brainstorm re-entry routing (ball returns to leader) ─────────────────

static void test_reentry_no_handoff_routes_to_leader() {
    std::cout << "\n=== D1. brainstorm non-leader no-HANDOFF → leader (not done) ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();
    auto conv_id = engine.start("Where is the most coupling?", 5.0, 20, "brainstorm");

    // Turn 1: leader → architect (discuss).
    auto t1 = h.latest_pending_task(conv_id);
    h.complete_task(t1);
    sui::quorum::ParsedOutput lead;
    lead.handoff = sui::quorum::HandoffBlock{.to = "architect",
                                             .prompt = "DISCUSS coupling — no write"};
    bool a1 = engine.on_task_complete(t1, lead, 0.05);
    check(a1, "D1: active after leader → architect");
    check(h.latest_pending_agent(conv_id) == "architect", "D1: architect has the ball");

    // Turn 2: architect discusses, emits NO HANDOFF. Pre-14.1 this went to
    // `done` (collapse). Now the ball must return to the leader.
    auto t2 = h.latest_pending_task(conv_id);
    h.complete_task(t2);
    sui::quorum::ParsedOutput arch;  // no handoff
    bool a2 = engine.on_task_complete(t2, arch, 0.05);
    check(a2, "D1: STILL active after architect no-HANDOFF (no collapse to done)");

    auto conv = h.db.get_conversation(conv_id);
    check(conv && conv->state == "active", "D1: conversation state == active");
    check(h.latest_pending_agent(conv_id) == "leader",
          "D1: ball returned to the LEADER (re-entry), not done");
}

static void test_reentry_handoff_to_leader_routes_to_leader() {
    std::cout << "\n=== D2. brainstorm non-leader HANDOFF→leader → leader ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();
    auto conv_id = engine.start("Discuss", 5.0, 20, "brainstorm");

    auto t1 = h.latest_pending_task(conv_id);
    h.complete_task(t1);
    sui::quorum::ParsedOutput lead;
    lead.handoff = sui::quorum::HandoffBlock{.to = "historian", .prompt = "discuss"};
    engine.on_task_complete(t1, lead, 0.05);

    auto t2 = h.latest_pending_task(conv_id);
    h.complete_task(t2);
    // Historian explicitly tries to hand back to the leader.
    sui::quorum::ParsedOutput hist;
    hist.handoff = sui::quorum::HandoffBlock{.to = "leader",
                                             .prompt = "done discussing, back to you"};
    bool a2 = engine.on_task_complete(t2, hist, 0.05);
    check(a2, "D2: active after historian HANDOFF→leader");
    check(h.latest_pending_agent(conv_id) == "leader",
          "D2: ball routed to the leader");
}

static void test_reentry_explicit_other_agent_unchanged() {
    std::cout << "\n=== D3. brainstorm non-leader HANDOFF→other knower unchanged ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();
    auto conv_id = engine.start("Discuss", 5.0, 20, "brainstorm");

    auto t1 = h.latest_pending_task(conv_id);
    h.complete_task(t1);
    sui::quorum::ParsedOutput lead;
    lead.handoff = sui::quorum::HandoffBlock{.to = "architect", .prompt = "discuss"};
    engine.on_task_complete(t1, lead, 0.05);

    auto t2 = h.latest_pending_task(conv_id);
    h.complete_task(t2);
    // Architect hands off to a SPECIFIC OTHER non-leader — must be honored
    // (re-entry only fires for no-HANDOFF or HANDOFF-to-leader).
    sui::quorum::ParsedOutput arch;
    arch.handoff = sui::quorum::HandoffBlock{.to = "historian",
                                             .prompt = "your lens next"};
    bool a2 = engine.on_task_complete(t2, arch, 0.05);
    check(a2, "D3: active after architect → historian");
    check(h.latest_pending_agent(conv_id) == "historian",
          "D3: explicit handoff to another knower is honored (not redirected to leader)");
}

static void test_leader_can_still_end_and_gate() {
    std::cout << "\n=== D4. leader gates (to: human); to: done only AFTER approval ===\n\n";

    // Phase 14.1b — in a GATED brainstorm a leader HANDOFF→done before the
    // human has approved is FORCE-CONVERTED into a waiting_for_human gate
    // (the daemon makes the gate unskippable). See test_forced_gate_* below
    // for the dedicated coverage; here we just confirm it does NOT complete.
    {
        TestHarness h;
        auto engine = h.make_engine();
        auto conv_id = engine.start("Discuss", 5.0, 20, "brainstorm");
        auto t1 = h.latest_pending_task(conv_id);
        h.complete_task(t1);
        sui::quorum::ParsedOutput done;
        done.handoff = sui::quorum::HandoffBlock{.to = "done"};
        bool a = engine.on_task_complete(t1, done, 0.05);
        check(!a, "D4: leader HANDOFF→done (gated, uncleared) does not stay active");
        auto conv = h.db.get_conversation(conv_id);
        check(conv && conv->state == "waiting_for_human",
              "D4: premature to:done force-converted to waiting_for_human (NOT done)");
    }
    // Leader → human parks at the gate.
    {
        TestHarness h;
        auto engine = h.make_engine();
        auto conv_id = engine.start("Discuss", 5.0, 20, "brainstorm");
        auto t1 = h.latest_pending_task(conv_id);
        h.complete_task(t1);
        sui::quorum::ParsedOutput gate;
        gate.handoff = sui::quorum::HandoffBlock{.to = "human", .prompt = "approve?"};
        bool a = engine.on_task_complete(t1, gate, 0.05);
        check(!a, "D4: leader HANDOFF→human parks the brainstorm");
        auto conv = h.db.get_conversation(conv_id);
        check(conv && conv->state == "waiting_for_human", "D4: state == waiting_for_human");
    }
}

// ─── F. forced gate: premature to:done can't complete a gated brainstorm ─────
//
// Phase 14.1b. The live-run failure: the leader engaged all four lenses,
// synthesized a capture proposal, then closed with `HANDOFF to: done` instead
// of `to: human`. The gate never fired, gate_cleared stayed 0, every knower
// write was suppressed, and the brainstorm captured NOTHING. The daemon now
// FORCE-CONVERTS that premature completion into a waiting_for_human gate. And
// because respond() clears the gate, a SUBSEQUENT to:done completes normally —
// at most one forced gate per conversation (no infinite loop).

static void test_forced_gate_converts_premature_done() {
    std::cout << "\n=== F1. gated brainstorm: leader to:done → waiting_for_human (forced) ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();
    auto conv_id = engine.start("Discuss design", 5.0, 20, "brainstorm");
    auto pre = h.db.get_conversation(conv_id);
    check(pre && pre->gated && !pre->gate_cleared,
          "F1: gated brainstorm, gate not yet cleared");

    // Leader synthesizes findings but (wrongly) closes with to:done.
    auto t1 = h.latest_pending_task(conv_id);
    h.complete_task(t1);
    sui::quorum::ParsedOutput done;
    done.handoff = sui::quorum::HandoffBlock{
        .to = "done", .prompt = "Consolidated findings + write manifest ..."};
    bool active = engine.on_task_complete(t1, done, 0.05);
    check(!active, "F1: conversation not active (held, not completed)");

    auto conv = h.db.get_conversation(conv_id);
    check(conv && conv->state == "waiting_for_human",
          "F1: premature to:done FORCE-CONVERTED to waiting_for_human (NOT done)");
    check(conv && conv->current_agent == "human",
          "F1: current_agent parked on human");
    check(conv && !conv->gate_cleared, "F1: gate still uncleared (awaiting approval)");
}

static void test_forced_gate_no_loop_after_clear() {
    std::cout << "\n=== F2. after approval, leader to:done completes (no infinite loop) ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();
    auto conv_id = engine.start("Discuss design", 5.0, 20, "brainstorm");

    // Premature to:done → forced gate.
    auto t1 = h.latest_pending_task(conv_id);
    h.complete_task(t1);
    sui::quorum::ParsedOutput done1;
    done1.handoff = sui::quorum::HandoffBlock{.to = "done", .prompt = "findings"};
    engine.on_task_complete(t1, done1, 0.05);
    {
        auto c = h.db.get_conversation(conv_id);
        check(c && c->state == "waiting_for_human", "F2: parked at forced gate");
    }

    // Human approves → gate_cleared = 1, conversation active again on the leader.
    bool ok = engine.respond(conv_id, "yes");
    check(ok, "F2: respond() accepted");
    {
        auto c = h.db.get_conversation(conv_id);
        check(c && c->gate_cleared, "F2: gate_cleared = 1 after respond");
        check(c && c->state == "active", "F2: active again after respond");
    }

    // Leader now closes with to:done — this time it COMPLETES (guard's
    // !gate_cleared is false). This proves the no-loop property: at most one
    // forced gate per conversation.
    auto t2 = h.latest_pending_task(conv_id);
    h.complete_task(t2);
    sui::quorum::ParsedOutput done2;
    done2.handoff = sui::quorum::HandoffBlock{.to = "done"};
    bool active2 = engine.on_task_complete(t2, done2, 0.05);
    check(!active2, "F2: leader to:done after approval ends the conversation");
    auto conv = h.db.get_conversation(conv_id);
    check(conv && conv->state == "done", "F2: state == done (no second forced gate)");
}

// ─── E. ungated single-knower scan still terminates ──────────────────────────
//
// run-knower.sh shape: a SINGLE knower runs an ungated brainstorm and emits its
// artifact + HANDOFF done. The explicit `to: done` must terminate even though
// the emitter is a non-leader (terminal handoff is honored; re-entry only fires
// for no-HANDOFF / HANDOFF-to-leader). And because the scan is ungated, the
// write would NOT be suppressed.

static void test_ungated_single_knower_scan_terminates() {
    std::cout << "\n=== E. ungated single-knower scan: HANDOFF done terminates + writes ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();
    // Ungated brainstorm (gated=0), as run-knower.sh launches it.
    auto conv_id = engine.start("Map the interconnections, emit map, HANDOFF done",
                                5.0, 20, "brainstorm",
                                /*no_vault_write=*/false, /*gated=*/0);
    auto conv0 = h.db.get_conversation(conv_id);
    check(conv0 && !conv0->gated, "E: scan is ungated (gated=0)");
    check(conv0 && !sui::quorum::brainstorm_gate_suppresses_write(
                       conv0->mode, conv0->gated, conv0->gate_cleared),
          "E: ungated scan write would NOT be suppressed");

    // The first agent IS the leader (start dispatches to leader). For a
    // single-knower scan run-knower.sh's roster has the knower as leader; here
    // we just assert the explicit `to: done` terminates from any emitter.
    auto t1 = h.latest_pending_task(conv_id);
    h.complete_task(t1);
    // Route to a knower first (simulating the knower turn), which then HANDOFFs
    // done with its artifact.
    sui::quorum::ParsedOutput lead;
    lead.handoff = sui::quorum::HandoffBlock{.to = "architect",
                                             .prompt = "produce the map, HANDOFF done"};
    engine.on_task_complete(t1, lead, 0.05);

    auto t2 = h.latest_pending_task(conv_id);
    h.complete_task(t2);
    sui::quorum::ParsedOutput scan;
    scan.handoff = sui::quorum::HandoffBlock{.to = "done"};
    // (In production the VAULT_UPDATE is applied at the main.cpp apply site;
    // here we assert the routing half — terminal handoff honored — which is the
    // engine's responsibility.)
    bool a2 = engine.on_task_complete(t2, scan, 0.05);
    check(!a2, "E: knower HANDOFF→done terminates the ungated scan");
    auto conv = h.db.get_conversation(conv_id);
    check(conv && conv->state == "done", "E: scan conversation state == done");
}

// ─── G. FIX B: non-leader to:done in a GATED brainstorm routes to leader ─────
//
// Phase 14.1c. The live-run bug: after the human cleared the gate, a NON-leader
// knower closed with `HANDOFF to: done` and ended the conversation early,
// skipping later lenses. In a GATED brainstorm only the LEADER may end. Even
// once the gate is cleared, a non-leader's to:done must bounce back to the
// leader (so later lenses still run) — the leader is the only one that ends.

static void test_reentry_nonleader_done_gated_routes_to_leader() {
    std::cout << "\n=== G. GATED brainstorm: non-leader to:done → leader (not done) ===\n\n";

    TestHarness h;
    auto engine = h.make_engine();
    auto conv_id = engine.start("Discuss design across lenses", 5.0, 20, "brainstorm");
    auto pre = h.db.get_conversation(conv_id);
    check(pre && pre->gated, "G: brainstorm is gated");

    // Clear the gate directly (as if a human already approved one round), so we
    // isolate the FIX B routing behaviour from the forced-gate (FIX 14.1b) path.
    h.db.set_gate_cleared(conv_id, true);
    {
        auto c = h.db.get_conversation(conv_id);
        check(c && c->gate_cleared, "G: gate_cleared set true (human approved)");
    }

    // Turn 1: leader → architect (discuss this lens).
    auto t1 = h.latest_pending_task(conv_id);
    h.complete_task(t1);
    sui::quorum::ParsedOutput lead;
    lead.handoff = sui::quorum::HandoffBlock{.to = "architect",
                                             .prompt = "discuss your lens"};
    engine.on_task_complete(t1, lead, 0.05);
    check(h.latest_pending_agent(conv_id) == "architect", "G: architect has the ball");

    // Turn 2: a NON-leader knower wrongly tries to end with HANDOFF to: done.
    // In a gated brainstorm only the leader ends → bounce back to the leader so
    // later lenses still run.
    auto t2 = h.latest_pending_task(conv_id);
    h.complete_task(t2);
    sui::quorum::ParsedOutput nl_done;
    nl_done.handoff = sui::quorum::HandoffBlock{.to = "done"};
    bool a2 = engine.on_task_complete(t2, nl_done, 0.05);
    check(a2, "G: STILL active after non-leader to:done in a gated brainstorm");

    auto conv = h.db.get_conversation(conv_id);
    check(conv && conv->state == "active",
          "G: conversation state == active (not done)");
    check(h.latest_pending_agent(conv_id) == "leader",
          "G: ball routed back to the LEADER (not done)");
}

// ─── H. DB round-trip for the staged-write table (FIX A) ─────────────────────

static void test_pending_vault_update_roundtrip() {
    std::cout << "\n=== H. stage / count / get(order) / clear pending_vault_updates ===\n\n";

    TestHarness h;
    auto conv_id = h.db.create_conversation("Stage test", 5.0, 20);

    h.db.stage_vault_update(conv_id, "architect", "thinker", "brainstorm",
                            "knowledge/coupling.md", "first note body");
    h.db.stage_vault_update(conv_id, "historian", "thinker", "brainstorm",
                            "knowledge/history.md", "second note body");

    check(h.db.count_pending_vault_updates(conv_id) == 2,
          "H: count_pending_vault_updates == 2 after two stages");

    auto pending = h.db.get_pending_vault_updates(conv_id);
    check(pending.size() == 2, "H: get_pending_vault_updates returns 2 rows");
    // Order by id (insertion order).
    check(pending[0].agent_id == "architect" &&
          pending[0].role == "thinker" &&
          pending[0].mode == "brainstorm" &&
          pending[0].path == "knowledge/coupling.md" &&
          pending[0].content == "first note body",
          "H: row 0 fields + order correct (architect first)");
    check(pending[1].agent_id == "historian" &&
          pending[1].path == "knowledge/history.md" &&
          pending[1].content == "second note body",
          "H: row 1 fields + order correct (historian second)");

    h.db.clear_pending_vault_updates(conv_id);
    check(h.db.count_pending_vault_updates(conv_id) == 0,
          "H: count == 0 after clear_pending_vault_updates");
    check(h.db.get_pending_vault_updates(conv_id).empty(),
          "H: get returns empty after clear");
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Brainstorm Gate + Re-entry Routing Tests (Phase 14.1) ===\n";

    test_gated_defaults();
    test_respond_clears_gate();
    test_suppression_invariant();
    test_suppression_across_gate_e2e();
    test_reentry_no_handoff_routes_to_leader();
    test_reentry_handoff_to_leader_routes_to_leader();
    test_reentry_explicit_other_agent_unchanged();
    test_leader_can_still_end_and_gate();
    test_forced_gate_converts_premature_done();
    test_forced_gate_no_loop_after_clear();
    test_ungated_single_knower_scan_terminates();
    test_reentry_nonleader_done_gated_routes_to_leader();
    test_pending_vault_update_roundtrip();

    std::cout << "\n--- Results: " << g_passed << "/" << (g_passed + g_failed)
              << " tests passed ---\n";

    return g_failed > 0 ? 1 : 0;
}
