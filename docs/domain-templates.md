# Quorum — Domain Templates

The four-role pattern (External Analysis, Internal Analysis, Building, Operating) applies to any domain with ongoing operations. This doc shows concrete mappings.

---

## The Pattern

Every domain has:

| Role | Core Question | Looks At |
|------|--------------|----------|
| **External Analyst** | "What's happening out there?" | Market, competitors, trends, opportunities |
| **Internal Analyst** | "How are we performing?" | Metrics, experiments, outcomes, optimization |
| **Builder** | "How do we implement it?" | Code, design, architecture, safety |
| **Operator** | "Is everything running?" | Processes, deployment, monitoring, incidents |

You can rename them, merge them (3 agents instead of 4), or split them further (5+ agents for complex domains). The framework doesn't care about names — it cares about distinct context requirements.

---

## Trading Operations (Default)

```
Market Analyst  → Pool metrics, competitor spreads, volume patterns, macro signals
Bot Analyst     → P&L, fill rates, parameter experiments, adverse selection
Engineer        → C++ bot code, architecture, safety rails, latency
Operator        → Process health, deployment, configs, incident response
```

**Typical proposal:** "Reduce base spread from 40 to 35 bps based on 2-week fill rate analysis"
**Review flow:** Bot Analyst creates → Engineer reviews safety → Operator deploys

---

## Software Engineering

```yaml
# agents/product_researcher.yaml
id: product_researcher
schedule:
  - type: periodic
    interval_minutes: 1440
    task: competitor_scan
  - type: periodic
    interval_minutes: 10080    # weekly
    task: user_feedback_digest
```

```
Product Researcher → User feedback (support tickets, NPS), competitor features, market trends
Code Quality Agent → Test coverage, bug rates, performance benchmarks, dependency health
Implementation Agent → Code writing, PR review, refactoring, design docs
DevOps Agent → CI/CD pipeline, monitoring, deployment, uptime, incident response
```

**Typical proposal:** "Refactor auth module — 3 high-severity bugs in last month, test coverage at 42%"
**Review flow:** Code Quality creates → Implementation Agent reviews feasibility → DevOps reviews deployment risk

**Data sources:**
- Product Researcher: Zendesk API, G2 reviews, competitor changelog feeds
- Code Quality: GitHub API (PR stats, test results), Sentry (error rates), Datadog
- Implementation Agent: Git repo, CI build logs, architecture docs
- DevOps: Kubernetes metrics, PagerDuty, deployment history

---

## Content Operations

```
Trend Analyst       → Social signals (Reddit, Twitter/X), SEO keyword trends, audience demographics
Performance Analyst → Engagement metrics (CTR, time-on-page), conversion rates, A/B test results
Content Creator     → Writing, editing, headline generation, asset creation
Publishing Agent    → Scheduling, distribution (email, social, RSS), format conversion
```

**Typical proposal:** "Shift editorial calendar toward AI-regulation content — search volume up 340% this month"
**Review flow:** Trend Analyst creates → Performance Analyst validates with engagement data → Content Creator assesses production capacity

---

## Research Lab

```
Literature Scanner   → New papers (arXiv, PubMed), related work, methodology trends
Experiment Analyst   → Results interpretation, statistical significance, replication assessment
Code Scientist       → Experiment implementation, compute management, data pipelines
Lab Manager          → Resource allocation, timeline tracking, grant reporting, compliance
```

**Typical proposal:** "Replicate Smith et al. 2026 with our dataset — their approach outperforms our baseline by 12%"
**Review flow:** Literature Scanner creates → Experiment Analyst validates methodology → Code Scientist estimates compute cost → Lab Manager checks resource availability

---

## Investment / Portfolio Management

```
Market Intelligence  → Macro indicators, sector rotation, earnings surprises, geopolitical events
Portfolio Analyst    → Position P&L, risk metrics (VaR, Sharpe), correlation analysis, drawdown tracking
Strategy Engineer    → Model implementation, backtesting, signal generation, execution logic
Operations Agent    → Trade execution monitoring, reconciliation, compliance reporting, broker connectivity
```

**Typical proposal:** "Increase tech sector allocation from 25% to 30% — momentum indicators positive across 5 timeframes"
**Review flow:** Market Intelligence creates → Portfolio Analyst reviews risk impact → Strategy Engineer validates against backtest → Human approves capital allocation

---

## DevOps / Infrastructure

```
Threat Monitor       → CVE feeds, dependency vulnerabilities, attack pattern analysis
Performance Analyst  → Latency percentiles, error rates, resource utilization, capacity planning
Platform Engineer    → Infrastructure-as-code, service mesh config, database optimization
Incident Commander   → Alert triage, runbook execution, post-mortem writing, on-call management
```

**Typical proposal:** "Migrate Redis cluster to version 7.4 — 2 CVEs in current version, one critical"
**Review flow:** Threat Monitor creates → Platform Engineer reviews migration plan → Performance Analyst validates benchmark results → Incident Commander confirms rollback procedure

---

## Adapting the Template

### Fewer Than 4 Agents

If two roles look at the same data, merge them:

```
# 3-agent setup for a small content team
Researcher → combines trend analysis + performance metrics
Creator    → content production
Publisher  → distribution + monitoring
```

### More Than 4 Agents

If one role has too many responsibilities, split it:

```
# 6-agent setup for a large trading desk
Market Macro Analyst  → cross-market signals, rates, FX
Market Micro Analyst  → per-venue order book analysis
Strategy Analyst      → signal backtesting, factor analysis
Risk Manager          → portfolio VaR, correlation, limits
Engineer              → execution code, connectivity
Operator              → deployment, monitoring, reconciliation
```

### The Merge/Split Test

Ask: "Does this agent need fundamentally different context (knowledge loaded into its prompt) than that agent?"

If yes → separate agents.
If no → merge into one agent with broader responsibilities.

Context separation is the point. Two agents looking at the same data from slightly different angles should be one agent.

---

## Quick Reference: Domain Mapping Table

| Domain | External | Internal | Builder | Operator |
|--------|----------|----------|---------|----------|
| Trading | Market Analyst | Bot Analyst | Engineer | Operator |
| Software | Product Researcher | Code Quality | Implementation | DevOps |
| Content | Trend Analyst | Performance Analyst | Creator | Publisher |
| Research | Literature Scanner | Experiment Analyst | Code Scientist | Lab Manager |
| Investment | Market Intelligence | Portfolio Analyst | Strategy Engineer | Operations |
| DevOps | Threat Monitor | Performance Analyst | Platform Engineer | Incident Commander |

This table demonstrates Quorum is **horizontal infrastructure**, not a single-use tool. The four-role pattern is the platform; domains are configurations.
