# Quorum — End-to-End Flow

> What it looks like when everything works.

This document walks through the complete Quorum experience from a cold start to a continuously operating multi-agent system with on-chain verification. The user has a Claude API key (or local LLM setup) and wants to run a team of AI agents that coordinate, accumulate knowledge, and make verifiable decisions.

---

## The 30-Second Mental Model

```
You define agents → Daemon runs them on schedule → Agents propose changes
→ Other agents review → Consensus reached → Action taken → Outcome measured
→ Knowledge accumulates → Agents get smarter → Repeat forever
```

The daemon is a loop. Agents are stateless LLM calls that read from their vault, think, and write back. The vault is their memory. The blockchain is the receipt.

---

## Phase 1: Setup (~10 minutes)

### Install

```bash
# Option A: Binary (macOS/Linux)
curl -fsSL https://quorum.dev/install.sh | sh

# Option B: npm (wraps the binary)
npm install -g @quorum/cli

# Option C: Build from source
git clone https://github.com/user/quorum && cd quorum
make build
```

### Initialize a Project

```bash
mkdir my-trading-agents && cd my-trading-agents
quorum init
```

This creates:

```
my-trading-agents/
├── quorum.yaml              # Daemon config (chain, walrus, inference settings)
├── agents/
│   ├── market_analyst.yaml  # Agent role definition
│   ├── bot_analyst.yaml
│   ├── engineer.yaml
│   └── operator.yaml
├── tasks/
│   ├── routine_scan.yaml    # Task definitions with prompt templates
│   └── deep_analysis.yaml
├── vaults/
│   ├── market_analyst/
│   │   └── CONTEXT.md       # Agent's role instructions (always loaded)
│   ├── bot_analyst/
│   │   └── CONTEXT.md
│   ├── engineer/
│   │   └── CONTEXT.md
│   └── operator/
│       └── CONTEXT.md
└── data/
    └── quorum.db            # SQLite (created on first run)
```

### Configure Credentials

```bash
# LLM access (pick one or both)
export ANTHROPIC_API_KEY=sk-ant-...          # Tier 2: frontier model
# Or configure local LLM in quorum.yaml:
#   inference.local.url: http://localhost:11434  (Ollama)

# Sui wallet (for on-chain features)
quorum wallet import --keystore ~/.sui/keystore
# Or generate new:
quorum wallet create

# Walrus (for persistent vault storage)
# Automatically configured for testnet; mainnet requires WAL tokens
```

At this point you have a working setup. No Sui wallet is strictly required — Quorum runs in **local-only mode** without on-chain features, storing everything in SQLite + local files. The blockchain layers are additive.

---

## Phase 2: Define Your Domain (~20 minutes)

### Customize Agent Roles

`quorum init` generates a four-agent template (Market Analyst, Bot Analyst, Engineer, Operator). You customize them for your domain by editing two things per agent:

**1. The YAML config** — what the agent does operationally:

```yaml
# agents/market_analyst.yaml
id: market_analyst
name: "Market Analyst"

schedule:
  - type: periodic
    interval_minutes: 30          # Check markets every 30 min
    task: routine_scan
  - type: periodic
    interval_minutes: 1440        # Deep dive once daily
    task: deep_analysis

triggers:
  - event: new_pool_detected      # React to external events
    task: pool_evaluation

inference_tier:
  routine_scan: 1                 # Local LLM (free, fast)
  deep_analysis: 2                # Frontier model ($$, thorough)
  pool_evaluation: 2

context_budget:
  max_vault_tokens: 50000         # How much vault knowledge to load
  always_include:
    - CONTEXT.md
    - knowledge/active-pools.md
```

**2. The vault CONTEXT.md** — who the agent is and what it knows:

```markdown
# Market Analyst — Agent Context

## Role
You discover and evaluate market-making opportunities in DeFi markets.

## You Look At
- Pool metrics, volume, fees, order book depth
- Competitor MM bots and their behavior
- Market regime (CALM / MODERATE / VOLATILE / CRASH)

## You Produce
- Pool analysis in knowledge/
- Opportunity proposals when actionable

## You Do NOT
- Write code, manage deployments, or tune parameters
```

### Customize Tasks

Tasks define what an agent does when triggered. Each task has inputs (where to get data), outputs (what to produce), and a prompt template:

```yaml
# tasks/routine_scan.yaml
id: routine_scan
agent: market_analyst
inference_tier: 1

inputs:
  - source: sqlite
    query: "SELECT * FROM pool_metrics WHERE timestamp > ..."
  - source: vault
    path: knowledge/active-pools.md

outputs:
  - type: vault_update       # Write findings to vault
  - type: proposal            # Create proposal if opportunity found

prompt_template: |
  You are the Market Analyst. Review the latest metrics...
  {vault_knowledge}
  {recent_metrics}
  ...respond with VAULT_UPDATE or PROPOSAL blocks.
```

### Seed Initial Knowledge (Optional)

Drop markdown files into agent vaults to give them starting knowledge:

```bash
# Give Market Analyst baseline pool info
echo "## Active Pools\n- DEEP/SUI: 1 bps fee, ~$50K daily volume" \
  > vaults/market_analyst/knowledge/active-pools.md

# Give Bot Analyst current parameters
echo "## Current Parameters\n- Base spread: 40 bps\n- Skew: 60 bps/unit" \
  > vaults/bot_analyst/knowledge/parameters.md
```

This is optional — agents build their vaults organically over time. But seeding accelerates the first few cycles.

---

## Phase 3: Start the Daemon

```bash
quorum daemon start
```

What happens:

```
[14:00:00] Quorum daemon starting...
[14:00:00] Loaded 4 agents: market_analyst, bot_analyst, engineer, operator
[14:00:00] Loaded 2 task definitions: routine_scan, deep_analysis
[14:00:00] SQLite database: data/quorum.db
[14:00:00] Sui network: testnet (package: 0xabc...)
[14:00:00] Walrus: enabled (testnet)
[14:00:00] Scheduler: 6 periodic tasks registered
[14:00:00] PID file: /tmp/quorum.pid
[14:00:00] Daemon running. Dashboard: http://localhost:7470
```

The daemon is now running a continuous loop. Here's what it does:

---

## Phase 4: The Continuous Loop

### 4a. Scheduled Agent Invocations

Every 30 minutes, the scheduler fires `routine_scan` for Market Analyst:

```
[14:30:00] SCHEDULER → routine_scan (market_analyst)
[14:30:00] CONTEXT_ASSEMBLER → Loading vault: CONTEXT.md, knowledge/active-pools.md
[14:30:00] CONTEXT_ASSEMBLER → Loading metrics: 47 rows from pool_metrics
[14:30:00] CONTEXT_ASSEMBLER → Prompt: 3,200 tokens
[14:30:00] MODEL_ROUTER → Tier 1 (local LLM: llama3.1:8b)
[14:30:02] INVOKER → Response: 850 tokens
[14:30:02] OUTPUT_PARSER → Parsed: 1 VAULT_UPDATE, 0 PROPOSAL, 1 SUMMARY
[14:30:02] VAULT → Updated: knowledge/active-pools.md (v12)
[14:30:02] AUDIT → Logged: agent_invocation (local, 0 cost)
```

The agent read its vault, looked at fresh metrics, updated its knowledge file, and found nothing worth proposing. This is the quiet majority of cycles — knowledge slowly accumulates.

### 4b. Agent Spots Something → Creates Proposal

Once daily, the deep analysis runs with a frontier model:

```
[02:00:00] SCHEDULER → deep_analysis (market_analyst)
[02:00:00] MODEL_ROUTER → Tier 2 (frontier: claude-sonnet-4-5)
[02:00:28] OUTPUT_PARSER → Parsed: 1 VAULT_UPDATE, 1 PROPOSAL
[02:00:28] PROPOSAL → Created: "Expand to SUI/USDC pool"
           Author: market_analyst
           Requires consensus: [bot_analyst, engineer]
           Informed: [operator]
           Status: DRAFT
```

The Market Analyst noticed SUI/USDC volume increased 3× this week with only one competitor. It wrote a proposal recommending expansion.

### 4c. Proposal Enters Review

The daemon detects a new proposal and routes it to the required reviewers:

```
[02:00:28] CONSENSUS → Proposal "Expand to SUI/USDC pool" → REVIEWING (round 1)
[02:00:28] ROUTER → Queuing review task for: bot_analyst
[02:00:28] ROUTER → Queuing review task for: engineer
```

**Bot Analyst reviews** (gets temporary read access to Market Analyst's relevant vault files via Seal):

```
[02:01:00] INVOKER → bot_analyst reviewing proposal
[02:01:15] OUTPUT_PARSER → Parsed: 1 REVIEW (verdict: REVISE)
           Feedback: "Volume data looks promising but we need to model
           expected fill rates at our spread levels. Request 48h dry-run
           data before committing capital."
```

**Engineer reviews:**

```
[02:02:00] INVOKER → engineer reviewing proposal
[02:02:20] OUTPUT_PARSER → Parsed: 1 REVIEW (verdict: REVISE)
           Feedback: "Config externalization supports multi-pool. Need to
           verify gas budget for parallel instances. No code blockers."
```

Both said REVISE. The proposal goes back to round 2.

### 4d. Proposal Iterates

Round 2 — Market Analyst revises based on feedback:

```
[02:05:00] INVOKER → market_analyst revising proposal (round 2)
[02:05:25] PROPOSAL → Updated: added dry-run plan, estimated fill rates,
           gas budget analysis from last 7 days
```

Bot Analyst and Engineer re-review:

```
[02:06:00] REVIEW → bot_analyst: APPROVE
           "Dry-run plan addresses my concern. 48h is sufficient."
[02:07:00] REVIEW → engineer: APPROVE
           "Gas analysis checks out. Config changes are straightforward."
```

### 4e. Consensus Reached

```
[02:07:00] CONSENSUS → All required reviewers approved
[02:07:00] PROPOSAL → Status: APPROVED
[02:07:01] SUI → Transaction: proposal state → APPROVED (tx: 0xdef...)
[02:07:01] AUDIT → On-chain: proposal-047 approved by bot_analyst, engineer
```

The approval is now recorded on Sui. The transaction hash is the cryptographic proof that these specific agents approved this specific proposal at this specific time.

### 4f. Human Approval Gate (When Required)

If the proposal involves capital deployment or going live, it's flagged for human approval:

```
[02:07:01] CONSENSUS → Human approval required (capital deployment)
[02:07:01] NOTIFICATION → Dashboard alert + terminal notification

┌──────────────────────────────────────────────────────────┐
│  🔔 PROPOSAL AWAITING YOUR APPROVAL                     │
│                                                          │
│  "Expand to SUI/USDC pool"                              │
│  Author: Market Analyst                                  │
│  Approved by: Bot Analyst ✓, Engineer ✓                  │
│                                                          │
│  Capital required: 1000 USDC + 50 SUI                   │
│  Risk: LOW (dry-run first, kill switches active)         │
│                                                          │
│  quorum proposal approve --id 047                        │
│  quorum proposal reject --id 047 --reason "..."          │
└──────────────────────────────────────────────────────────┘
```

You approve from CLI or dashboard. Your wallet signature IS the approval — recorded on Sui:

```bash
quorum proposal approve --id 047
# Signs a Sui transaction with your wallet
```

```
[09:15:00] HUMAN → Approved proposal-047 (tx: 0xfed...)
[09:15:00] PROPOSAL → Status: APPROVED (human gate cleared)
```

### 4g. Execution

The Operator agent picks up the approved proposal:

```
[09:15:01] ROUTER → Execution task → operator
[09:15:10] OPERATOR → Executing: generate SUI/USDC config, deploy dry-run instance
[09:15:10] PROPOSAL → Status: EXECUTED
[09:15:11] SUI → Transaction: proposal state → EXECUTED (tx: 0x123...)
```

### 4h. Outcome Evaluation (48 hours later)

The daemon schedules an automatic follow-up:

```
[48h later]
[09:15:00] SCHEDULER → outcome_evaluation (bot_analyst)
[09:15:00] INVOKER → bot_analyst evaluating proposal-047 outcomes
[09:15:20] OUTPUT_PARSER → Parsed: 1 OUTCOME_EVALUATION

  Prediction: "SUI/USDC fill rate ~2× higher than DEEP/SUI"
  Actual:     "SUI/USDC fill rate 2.3× higher (exceeded expectation)"
  Prediction: "One competitor, spreads 30-50 bps"
  Actual:     "Two competitors, spreads 25-45 bps (more competitive than expected)"
  Overall:    "Directionally correct, magnitude slightly conservative"

[09:15:20] PROPOSAL → Status: EVALUATED
[09:15:21] SUI → Transaction: outcome recorded (tx: 0x456...)
[09:15:21] WALRUS → Stored: full evaluation report (blob: walrus_xyz...)
[09:15:21] VAULT → bot_analyst/decisions/proposal-047-eval.md updated
```

The prediction vs. actual comparison is now permanently recorded. Over time, this builds each agent's **verifiable track record**.

---

## Phase 5: Knowledge Accumulation (Weeks → Months)

This is where Quorum diverges from every other framework. After weeks of operation:

### Agent Vaults Grow Organically

```
vaults/market_analyst/
├── CONTEXT.md
├── knowledge/
│   ├── active-pools.md          # Updated 847 times
│   ├── competitor-map.md        # 12 competitors tracked
│   ├── volume-patterns.md       # "SUI/USDC volume 2× on weekdays"
│   ├── fee-tier-analysis.md     # "1 bps pools have 4× more MM competition"
│   └── regime-correlations.md   # "BTC drawdowns >5% → VOLATILE within 2h"
├── experiments/
│   └── (empty — Market Analyst doesn't run experiments)
└── decisions/
    ├── proposal-012-eval.md     # "Correctly predicted DEEP/USDC opportunity"
    ├── proposal-031-eval.md     # "Overestimated volume on SUI/USDT"
    └── proposal-047-eval.md     # "SUI/USDC expansion — conservative but correct"
```

```
vaults/bot_analyst/
├── CONTEXT.md
├── knowledge/
│   ├── parameters.md            # Current optimal params + reasoning
│   ├── spread-analysis.md       # "35 bps optimal for CALM, 55 for MODERATE"
│   ├── fill-rate-model.md       # Empirical fill rate vs spread curve
│   └── gas-efficiency.md        # "Refresh hysteresis saves 92% gas"
├── experiments/
│   ├── exp-001-spread-35bps.md  # Completed, positive result
│   ├── exp-002-two-levels.md    # Completed, negative (gas too high)
│   ├── exp-003-skew-80bps.md    # Completed, mixed results
│   └── exp-004-size-doubling.md # In progress
└── decisions/
    ├── proposal-008-eval.md
    └── ... (35 evaluated proposals)
```

Each agent's vault is its **institutional memory**. The Market Analyst "knows" that volume spikes on weekdays not because someone told it, but because it observed the pattern across hundreds of data points and wrote the conclusion itself.

### Agent Track Records Build On-Chain

```bash
quorum agent stats

┌────────────────────────────────────────────────────────────┐
│  Agent Track Records (on-chain verified)                   │
│                                                            │
│  market_analyst                                            │
│    Proposals: 23 created, 19 approved, 18 evaluated        │
│    Accuracy (directional): 83%                             │
│    Avg outcome improvement: +6.2% revenue per proposal     │
│                                                            │
│  bot_analyst                                               │
│    Proposals: 47 created, 38 approved, 35 evaluated        │
│    Accuracy (directional): 82%                             │
│    Accuracy (magnitude ±20%): 61%                          │
│    Avg outcome improvement: +8.3% revenue per proposal     │
│                                                            │
│  engineer                                                  │
│    Reviews: 52 submitted                                   │
│    Proposals: 8 created (design changes)                   │
│    Safety catches: 4 (prevented problematic parameter sets)│
│                                                            │
│  operator                                                  │
│    Deployments: 31 executed                                │
│    Incidents: 3 detected, all resolved < 15 min            │
│    Uptime: 99.7%                                           │
└────────────────────────────────────────────────────────────┘
```

These aren't self-reported metrics. Every number traces back to an on-chain transaction.

---

## Phase 6: Steady State — What a Typical Day Looks Like

```
00:00  Operator health check (Tier 1, local) ─────────── 2s, $0
00:15  Operator health check
00:30  Market Analyst routine scan (Tier 1, local) ───── 3s, $0
00:30  Operator health check
00:45  Operator health check
01:00  Bot Analyst performance review (Tier 1, local) ── 4s, $0
01:00  Operator health check
...
02:00  Market Analyst deep analysis (Tier 2, Claude) ─── 28s, $0.05
02:00  Bot Analyst daily P&L (Tier 2, Claude) ────────── 22s, $0.04
       → Bot Analyst creates proposal: "Widen spread to 45 bps for MODERATE"
02:01  CONSENSUS: proposal enters review
02:02  Engineer reviews (Tier 2) ─────────────────────── 15s, $0.03
       → APPROVE: "Kill switch thresholds still valid at 45 bps"
02:02  CONSENSUS: approved → EXECUTED
02:02  Sui tx: proposal-089 APPROVED (0.001 SUI)
02:02  Walrus: proposal content stored (0.001 WAL)
...
[remaining 22 hours: 48 local LLM calls, 0 frontier calls, ~$0]

Daily totals:
  Agent invocations: 58 (52 local, 6 frontier)
  Proposals: 1 created, 1 approved, 1 from yesterday evaluated
  Sui transactions: 4
  Walrus writes: 12 vault updates
  Cost: ~$0.35
```

Most of the day is quiet — local LLM calls that cost nothing, updating vault knowledge incrementally. The expensive frontier calls happen once or twice daily for deep analysis. On-chain activity is minimal: a few transactions for proposal lifecycle and audit entries.

---

## The Dashboard

The web dashboard (`http://localhost:7470` or `@quorum/dashboard`) shows:

### Proposals View
- Active proposals with current status and round
- Review history (who said what, when)
- Linked Sui transactions (click to verify on explorer)
- Outcome evaluations with prediction accuracy

### Vaults View
- Browse each agent's knowledge files
- Version history (when was this file last updated? what changed?)
- Walrus blob IDs for verification
- Token count per file (context budget usage)

### Agents View
- Agent status (last invocation, next scheduled)
- Track record (proposals, accuracy, outcomes)
- Inference tier usage (% local vs frontier)
- Cost breakdown per agent

### Audit View
- Complete event timeline
- Filter by agent, event type, date range
- On-chain verification links for every decision event
- Exportable for compliance or debugging

---

## CLI Quick Reference

```bash
# Daemon
quorum daemon start              # Start daemon (foreground)
quorum daemon start -d           # Start daemon (background)
quorum daemon stop               # Graceful shutdown
quorum daemon status             # Running? Last heartbeat? Agent count?

# Agents
quorum agent list                # All agents with status
quorum agent invoke market_analyst routine_scan   # Manual trigger
quorum agent stats               # Track records

# Proposals
quorum proposal list             # All proposals with status
quorum proposal create --title "..." --author bot_analyst
quorum proposal status --id 047  # Detailed view with reviews
quorum proposal approve --id 047 # Human approval (signs Sui tx)
quorum proposal reject --id 047 --reason "..."
quorum proposal history          # Completed proposals with outcomes

# Vaults
quorum vault list --agent market_analyst
quorum vault read --agent market_analyst --path knowledge/active-pools.md
quorum vault search --query "SUI/USDC volume"

# Audit
quorum audit list --limit 20
quorum audit verify --tx 0xdef...  # Verify on-chain

# Config
quorum config show               # Current daemon config
quorum config validate           # Check config for errors
```

---

## What If: Common Scenarios

### "I want to add a fifth agent"

```bash
# Create agent config
cat > agents/risk_manager.yaml << EOF
id: risk_manager
name: "Risk Manager"
schedule:
  - type: periodic
    interval_minutes: 60
    task: risk_assessment
inference_tier:
  risk_assessment: 2
EOF

# Create vault
mkdir -p vaults/risk_manager/knowledge
cat > vaults/risk_manager/CONTEXT.md << EOF
# Risk Manager
You monitor portfolio-level risk across all running bots...
EOF

# Register on-chain
quorum agent create --config agents/risk_manager.yaml

# Restart daemon to pick up new agent
quorum daemon restart
```

### "I want to use this for software engineering, not trading"

Change the CONTEXT.md files and agent configs. The four-role pattern maps:

```
Market Analyst  →  Product Researcher
Bot Analyst     →  Code Quality Analyst
Engineer        →  Implementation Agent
Operator        →  DevOps Agent
```

Edit each vault's CONTEXT.md to describe the new domain. Edit task YAML files with appropriate data sources and prompt templates. The daemon, proposal protocol, and vault system work identically.

### "An agent proposed something dangerous"

The Engineer agent is designed to catch this in review. But if a bad proposal reaches APPROVED:

```bash
# Human override — reject at any point
quorum proposal reject --id 089 --reason "Risk too high" --override

# Emergency: stop all agent activity
quorum daemon pause    # Stops scheduling, finishes current invocations
quorum daemon resume   # Resume when ready
```

The kill switches in the underlying system (mm-bot) are independent of Quorum. Even if the orchestrator makes a bad decision, the bot's C++ safety rails (capital loss limits, inventory skew limits, staleness checks) prevent catastrophic outcomes.

### "Chain is down / Walrus is unreachable"

Quorum continues operating locally. Decisions queue up:

```
[14:30:00] WARN: Sui RPC unreachable — queuing on-chain operations
[14:30:00] Proposal-090 approved (local state updated, chain sync pending)
[14:30:00] Vault updates written to local cache (Walrus sync pending)
...
[15:45:00] INFO: Sui RPC reconnected — syncing 3 queued transactions
[15:45:02] INFO: Walrus reconnected — syncing 8 vault updates
```

Local speed, on-chain truth. The local system is the source of speed; the chain is the source of truth.

---

## Failure Modes

| Scenario | What Happens |
|----------|-------------|
| LLM API down | Daemon keeps running, agent invocations fail gracefully, retry on next schedule tick |
| Sui RPC down | Proposals track locally in SQLite, on-chain sync queues until reconnection |
| Walrus unreachable | Vault writes to local files, Walrus sync retries in background |
| Bad proposal approved | Human override via CLI (`proposal reject --override`), underlying system kill switches independent of Quorum |
| Agent producing garbage | Operator pauses daemon, reviews CONTEXT.md, adjusts instructions, resumes |
| Consensus deadlock | 3-round limit → automatic human escalation |

---

## Open Design Questions

1. **Notification system:** How does the operator learn about human-gated proposals? Dashboard polling? Push notification? Telegram/Slack bot?
2. **Multi-operator:** Can multiple humans share approval authority? Threshold approval (2 of 3)?
3. **Agent hot-reload:** Can CONTEXT.md changes take effect without daemon restart?
4. **Vault conflict resolution:** If operator manually edits a vault file that an agent also updates, who wins?
5. **Proposal dependencies:** Can proposal B declare "only execute after proposal A succeeds"?
