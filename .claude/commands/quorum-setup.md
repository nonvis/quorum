# /quorum-setup — Interactive Project Scaffolder

You are setting up a new Quorum project. Quorum is a C++20 daemon that orchestrates Claude Code CLI agents. Each project has a config YAML + agent configs + vault directories.

## Mode

Ask the user: **"Set up an existing project from its config, or create a new project from scratch?"**

### Mode A: Set up from existing config

1. Ask which config: `ls configs/*.yaml` to show options
2. Read the chosen config to discover agents (`agents: - config: ...` entries)
3. For each agent, read its YAML to get `id`, `vault_path`, `context_file`, `agent_class`, `target_dir`
4. Create missing directories and files (see Setup Steps below)

### Mode B: Create new project

Ask these questions one at a time:

1. **Project name** — lowercase-hyphenated (e.g., `hello-world`, `code-reviewer`)
2. **Description** — one sentence, what does this project do?
3. **Pipeline type** — `analyst` (Thinker → Reviewer → Done) or `executor` (Thinker → Human Gate → Executor → Reviewer → Done)?
4. **Agents** — for executor pipeline, default is thinker + executor + reviewer. Ask if they want to customize names or add agents.
5. **Target directory** (executor pipeline only) — where should the executor agent work? (e.g., `~/projects/myapp`)

Then create all configs + data.

## Setup Steps

For each agent discovered or defined:

### 1. Vault directories
```bash
mkdir -p data/vaults/{agent_id}/knowledge
```

### 2. CONTEXT.md
If `data/vaults/{agent_id}/CONTEXT.md` doesn't exist:
- For **analyst** agents: use `docs/templates/CONTEXT_TEMPLATE.md` as base, fill in agent name and role
- For **executor** agents: create a minimal CONTEXT.md focused on implementation (no Output Rules section — executors write files directly)
- For **reviewer** agents in executor pipeline: create a review-focused CONTEXT.md (check plan vs implementation)

### 3. Target directory (executor agents only)
If the agent YAML has `target_dir`, create it:
```bash
mkdir -p {expanded_target_dir}
```

### 4. Project config YAML (new projects only)
Create `configs/{project-name}.yaml` following the pattern in `configs/hello-world.yaml`:
- `daemon.pid_file`: `/tmp/quorum-{project-name}.pid`
- `daemon.data_dir`: `./data`
- `budget`: conservative defaults (daily $5, hourly $2, timeout 120s)
- `conversations`: set pipeline, agent role names
- `agents`: list of `- config:` paths

### 5. Agent config YAMLs (new projects only)
Create `configs/agents/{project-name}/{agent_id}.yaml` for each agent.
Follow the pattern in `configs/agents/hello-world/` for executor pipeline agents, or `configs/agents/mm-bot/` for analyst agents.

## Validation

After setup, verify:
1. All vault directories exist
2. All CONTEXT.md files exist and are non-empty
3. All agent YAML files referenced in the project config exist
4. Target directory exists (if applicable)
5. Config loads: `./build/quorum_daemon --config configs/{project}.yaml --help` (should not error)

## Reference

- Existing project configs: `configs/*.yaml`
- Agent config examples: `configs/agents/mm-bot/`, `configs/agents/hello-world/`
- CONTEXT.md template: `docs/templates/CONTEXT_TEMPLATE.md`
- Vault structure: `data/vaults/{agent_id}/` with `CONTEXT.md`, `knowledge/`

## Rules

- Be idempotent — skip files/dirs that already exist, report what was skipped
- Never overwrite existing CONTEXT.md files (they may contain operator-authored content)
- Never modify existing agent YAMLs
- Always use the same YAML style as existing configs (no quotes on simple strings, 2-space indent)
