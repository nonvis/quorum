# /quorum-setup — Interactive Project Scaffolder

You are setting up a new Quorum project. Quorum is a C++20 daemon that orchestrates Claude Code CLI agents in a team. Each project has a config YAML + agent configs + vault directories.

## Mode

Ask the user: **"Set up an existing project from its config, or create a new project from scratch?"**

### Mode A: Set up from existing config

1. Ask which config: `ls configs/*.yaml` to show options
2. Read the chosen config to discover agents (`agents: - config: ...` entries)
3. For each agent, read its YAML to get `id`, `role`, `vault_path`, `context_file`, `agent_class`, `target_dir`
4. Create missing directories and files (see Setup Steps below)

### Mode B: Create new project

Ask these questions one at a time:

1. **Project name** — lowercase-hyphenated (e.g., `hello-world`, `code-reviewer`)
2. **Description** — one sentence, what does this project do?
3. **Team composition** — which agent roles does this project need?
   - Available roles: `leader`, `thinker`, `doer`, `reviewer`, `scribe`, `librarian`
   - Role determines tool access: `doer` = executor (full tools), all others = analyst (read-only)
   - Minimum viable team: `thinker` + `doer` (thinker plans, doer executes)
   - Common setups:
     - **Analysis only**: `thinker` (+ optional `reviewer`)
     - **Build team**: `thinker` + `doer` (+ optional `reviewer`)
     - **Full team**: `leader` + `thinker` + `doer` + `reviewer` + `scribe`
4. **Agent names** — for each role, ask for a descriptive agent name (e.g., `move-dev` for a doer, `arch-reviewer` for a reviewer)
5. **Target directory** (doer agents only) — where should the doer agent work? (e.g., `~/projects/myapp`)

Then create all configs + data.

## Setup Steps

For each agent discovered or defined:

### 1. Vault directories
```bash
mkdir -p data/vaults/{agent_id}/knowledge
```

### 2. CONTEXT.md
If `data/vaults/{agent_id}/CONTEXT.md` doesn't exist:
- Copy `docs/templates/CONTEXT_TEMPLATE.md` as base
- Select the correct Output Rules variant based on the agent's role:
  - **Analyst-class** (leader, thinker, reviewer, scribe, librarian): Variant A (read-only, structured blocks)
  - **Doer-class** (doer): Variant B (full tool access)
- Fill in agent name, role, and description
- Remove the unused variant and template comment blocks

### 3. Target directory (doer agents only)
If the agent YAML has `target_dir`, create it:
```bash
mkdir -p {expanded_target_dir}
```

### 4. Project config YAML (new projects only)
Create `configs/{project-name}.yaml` following the pattern in existing configs:
- `daemon.pid_file`: `/tmp/quorum-{project-name}.pid`
- `daemon.data_dir`: `./data`
- `budget`: conservative defaults (daily $5, hourly $2, timeout 120s)
- `agents`: list of `- config:` paths

### 5. Agent config YAMLs (new projects only)
Create `configs/agents/{project-name}/{agent_id}.yaml` for each agent:
```yaml
id: {agent_id}
name: "{Agent Display Name}"
role: {role}
agent_class: {executor|analyst}
description: "{One sentence description}"
vault_path: data/vaults/{agent_id}/
context_file: data/vaults/{agent_id}/CONTEXT.md
# skill_file: path/to/SKILL.md  # optional
# executor section only for doer agents:
# executor:
#   target_dir: ~/path/to/target
```

### 6. Scribe processing instructions (if scribe agent exists)
If the team includes a scribe agent:
- Copy `docs/templates/PROCESSING_TEMPLATE.md` to `data/vaults/{scribe_id}/PROCESSING.md`
- Adjust domain-specific content as needed

## Validation

After setup, verify:
1. All vault directories exist
2. All CONTEXT.md files exist and are non-empty
3. All agent YAML files referenced in the project config exist
4. Target directory exists (if applicable)
5. Config loads: `./build/quorum_daemon --config configs/{project}.yaml --help` (should not error)

## Reference

- Existing project configs: `configs/*.yaml`
- Agent config examples: `configs/agents/`
- CONTEXT.md template: `docs/templates/CONTEXT_TEMPLATE.md`
- Scribe processing template: `docs/templates/PROCESSING_TEMPLATE.md`
- Vault structure: `data/vaults/{agent_id}/` with `CONTEXT.md`, `knowledge/`

## Rules

- Be idempotent — skip files/dirs that already exist, report what was skipped
- Never overwrite existing CONTEXT.md files (they may contain operator-authored content)
- Never modify existing agent YAMLs
- Always use the same YAML style as existing configs (no quotes on simple strings, 2-space indent)
