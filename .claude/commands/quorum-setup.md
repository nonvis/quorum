# /quorum-setup — Interactive Project Scaffolder

You are setting up a new Quorum project. Quorum is a C++20 daemon that orchestrates Claude Code CLI agents in a team. Each project has a `.quorum/` directory with config, agent YAMLs, vaults, and a SQLite database.

## Mode

Ask the user: **"Set up an existing `.quorum/` project (verify + fix), or create a new project from scratch?"**

### Mode A: Verify existing project

1. Check `.quorum/config.yaml` exists
2. Read the config to discover settings
3. Scan `.quorum/agents/` for agent YAMLs — read each to get `id`, `role`, `vault_path`, `context_file`, `agent_class`, `target_dir`
4. Create missing directories and files (see Setup Steps below)

### Mode B: Create new project

Ask these questions one at a time:

1. **Project directory** — where to initialize (default: current directory)
2. **Description** — one sentence, what does this project do?
3. **Team composition** — which agent roles does this project need?
   - Available roles: `leader`, `thinker`, `doer`, `scribe`, `librarian`, `evaluator`
   - Role determines tool access: `doer` = executor (full tools), all others = analyst (read-only)
   - Review/check is not a core role — it's a `thinker`-role review specialty (`--role thinker` + a review skill)
   - Minimum viable team: `thinker` + `doer` (thinker plans, doer executes)
   - Common setups:
     - **Analysis only**: `thinker` (+ optional `thinker` review specialty)
     - **Build team**: `thinker` + `doer` (+ optional `thinker` review specialty)
     - **Full team**: `leader` + `thinker` + `doer` + a `thinker` review specialty + `scribe`
4. **Agent names** — for each role, ask for a descriptive agent name (e.g., `move-dev` for a doer, `arch-reviewer` for a `thinker` review specialty)
5. **Target directory** (doer agents only) — where should the doer agent work? (e.g., `.` for project root, `~/projects/myapp`)

Then run `quorum init` + `quorum agent create` for each agent.

## Setup Steps

### 1. Initialize project

```bash
cd {project_dir} && quorum init
```

This creates `.quorum/` with config.yaml, quorum.db (schema pre-created), agents/, vaults/, and provisions the full default roster (leader + 4 knowers + scribe + thinker).

### 2. Create agents

For each agent:

```bash
# Template-based (no AI, uses templates/agents/{role}.md)
quorum agent create --role {role} --name {agent_name} --no-ai

# Doer agents need --target-dir
quorum agent create --role doer --name {agent_name} --target-dir {path} --no-ai
```

This scaffolds:
- `.quorum/agents/{agent_name}.yaml` — agent config
- `.quorum/vaults/{agent_name}/CONTEXT.md` — agent identity
- `.quorum/vaults/{agent_name}/knowledge/` — knowledge directory
- Auto-detects role skill from `~/.claude/skills/quorum-roles/{role}/SKILL.md`

### 3. Install role skills (if not already installed)

```bash
./scripts/install-skills.sh        # copy to ~/.claude/skills/
./scripts/install-skills.sh --link # or symlink for development
```

### 4. Edit CONTEXT.md (optional)

Review and customize agent identity files:

```bash
vim .quorum/vaults/{agent_name}/CONTEXT.md
```

Or use the web dashboard's CONTEXT.md editor (click any agent badge).

### 5. Add optional agents (optional)

`quorum init` already provisions the full default roster (leader + 4 knowers + scribe + thinker). Add optional agents (doers, advisor, a `thinker` review specialty, evaluator) as the project needs them:

```bash
# Add a doer for a given working directory
quorum agent create --role doer --name {agent_name} --target-dir {path} --no-ai

# Or use the setup helpers for common rosters
./scripts/setup-knowers.sh
```

There are no team presets to define — the leader routes each goal to the best-fit agents across the full roster at conversation time.

## Validation

After setup, verify:
1. `.quorum/config.yaml` exists and is valid
2. `.quorum/agents/` contains a YAML for each agent
3. `.quorum/vaults/{agent}/CONTEXT.md` exists and is non-empty for each agent
4. Target directory exists (if applicable)
5. Role skills installed: `ls ~/.claude/skills/quorum-roles/`
6. Daemon loads: `./build/quorum_daemon --help`

## Reference

- Agent YAML examples: `.quorum/agents/` (after creation)
- Agent CONTEXT.md templates: `templates/agents/`
- Role skills: `templates/skills/quorum-roles/`
- Domain skills: `templates/skills/sui-dev-skills/`, `templates/skills/move-code-quality/`
- Vault structure: `.quorum/vaults/{agent_id}/` with `CONTEXT.md`, `knowledge/`

## Rules

- Be idempotent — skip files/dirs that already exist, report what was skipped
- Never overwrite existing CONTEXT.md files (they may contain operator-authored content)
- Never modify existing agent YAMLs
- Always use the same YAML style as existing configs (no quotes on simple strings, 2-space indent)
