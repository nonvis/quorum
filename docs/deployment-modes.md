# Quorum — Deployment Modes

Quorum operates across a spectrum. You don't need the full Sui Stack to start — features layer on incrementally.

---

## Mode 1: Local Only

**Requirements:** LLM access (API key or local Ollama)
**Blockchain:** None
**Storage:** Local files + SQLite

```yaml
# quorum.yaml
chain:
  enabled: false
walrus:
  enabled: false
seal:
  enabled: false
```

What works:
- ✅ Daemon scheduling, routing, consensus (all local)
- ✅ Agent invocations (local or frontier LLM)
- ✅ Vault knowledge accumulation (local markdown files)
- ✅ Proposal protocol (tracked in SQLite)
- ✅ CLI and dashboard
- ❌ On-chain audit trail
- ❌ Tamper-proof decision records
- ❌ Cryptographic cross-agent access control
- ❌ Decentralized vault persistence

**Good for:** Trying Quorum out, development, domains where verifiability doesn't matter.

---

## Mode 2: Local + Sui (Audit Trail)

**Requirements:** LLM access + Sui wallet (testnet is free)
**Blockchain:** Sui testnet or mainnet
**Storage:** Local files + SQLite + Sui transactions

```yaml
chain:
  enabled: true
  network: testnet
walrus:
  enabled: false
seal:
  enabled: false
```

What this adds:
- ✅ Proposal state transitions recorded on-chain
- ✅ Agent identities registered on-chain
- ✅ Audit log entries with Sui transaction hashes
- ✅ Human approval via wallet signature

Vaults are still local files. If your disk dies, vault knowledge is gone. But every decision is on-chain and verifiable.

**Good for:** When you need auditable decision records but don't need decentralized storage.

---

## Mode 3: Local + Sui + Walrus (Persistent Vaults)

**Requirements:** LLM access + Sui wallet + WAL tokens (small amount)
**Blockchain:** Sui + Walrus
**Storage:** Local cache + Walrus blobs + Sui transactions

```yaml
chain:
  enabled: true
  network: testnet
walrus:
  enabled: true
seal:
  enabled: false
```

What this adds:
- ✅ Vault files persisted on Walrus (content-addressed, versioned)
- ✅ Vault survives local disk failure
- ✅ Version history for every knowledge file
- ✅ Blob IDs provide tamper-evidence

Cross-agent vault reads are still unrestricted (no Seal). Any agent could theoretically read another's vault.

**Good for:** When vault persistence matters (weeks/months of accumulated knowledge).

---

## Mode 4: Full Stack (Sui + Walrus + Seal)

**Requirements:** LLM access + Sui wallet + WAL tokens + Seal policies deployed
**Blockchain:** Sui + Walrus + Seal
**Storage:** Local cache + encrypted Walrus blobs + Sui transactions

```yaml
chain:
  enabled: true
  network: mainnet
walrus:
  enabled: true
seal:
  enabled: true
```

What this adds:
- ✅ Vault files encrypted with Seal before Walrus upload
- ✅ Cross-agent reads only through authorized proposal review
- ✅ Access policies are Move smart contracts (not config)
- ✅ Threshold encryption (no single point of compromise)

**Good for:** Production financial systems, multi-party trust scenarios, anything where agent knowledge boundaries must be cryptographically enforced.

---

## Choosing a Mode

| Question | If Yes → |
|----------|----------|
| Am I just trying this out? | Mode 1 |
| Do I need to prove decisions were made correctly? | Mode 2+ |
| Could I lose weeks of agent knowledge and not recover? | Mode 3+ |
| Do multiple untrusted parties interact with the system? | Mode 4 |
| Am I applying for a Sui Foundation grant? | Mode 4 |

You can upgrade modes at any time. Moving from Mode 1 → 2 requires deploying Move contracts and registering agents. Moving from 2 → 3 requires a one-time vault sync to Walrus. Moving from 3 → 4 requires encrypting existing vault blobs with Seal policies.

No data is lost during upgrades. Each mode is a strict superset of the previous one.

---

## Stakeholder Implications by Mode

| Stakeholder | Mode 1 | Mode 2 | Mode 3 | Mode 4 |
|-------------|--------|--------|--------|--------|
| **Operator** | Full control, no chain setup | On-chain audit trail | Vault backup survives disk loss | Cryptographic agent boundaries |
| **Observer** | No external verification possible | Can verify decisions on Sui explorer | Can read vault content via Walrus | Can verify access was policy-controlled |
| **Developer** | Fast local iteration, no dependencies | Test on-chain with testnet (free) | Test vault persistence | Test full access control flow |
