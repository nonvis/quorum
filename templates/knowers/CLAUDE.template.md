# <Workspace name>

This folder is a workspace analyzed by read-only Quorum "knower" agents. It may
be a single repo or a multi-repo workspace (several independent git repos under
one root, where the root itself is **not** a git repo). The authoritative layout
and architecture maps are produced by the Quorum agents — see below.

## ⛔ DO NOT COMMIT OR PUSH — EVER

**No AI tool (Claude Code, Quorum agents, anything) may run `git add`, `git commit`, `git push`, `git checkout`, `git reset`, branch changes, or any state-mutating git command in this workspace or in ANY sub-repo.**

- The user performs **all** git operations manually.
- Read-only git is fine: `git log`, `git show`, `git diff`, `git status`, `git branch --show-current`.
- This applies to every sub-repo regardless of its branch or dirty state.
- If a task seems to need a commit, **stop and ask the user** — do not commit.

The Quorum agents pointed at this workspace are **analysts (read-only)** and run in **brainstorm mode** for analysis runs, which clamps them to `Read/Grep/Glob` (no Bash, no writes) — but this rule stands regardless.

## Folders

Describe your top-level folders here, one line each (the operator fills this in;
the `cartographer` agent then produces the authoritative, verified index into
`.quorum/vaults/cartographer/knowledge/ref-project-index.md`):

| Folder / repo | Lang | Purpose (provisional) |
|---------------|------|------------------------|
| `<folder>`    | `?`  | `<describe>`          |

The provisional descriptions above are best-effort. The **authoritative**
layout index is what the Quorum `cartographer` agent produces, and the
component interconnection map is what the `architect` agent produces, both
under `.quorum/vaults/<agent>/knowledge/`.

## Quorum workspace

`.quorum/` here holds read-only "knower" agents — the `cartographer` (maps the
project layout / "where is X") and the `architect` (maps component
interconnections with file evidence). Everything Quorum writes stays under
`.quorum/`, never inside a sub-repo. The knowers never modify the repos.
