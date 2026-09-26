# Setting up Quorum on a machine

The runbook for putting Quorum on a new machine (macOS, Linux, or WSL2) and for
bringing an existing Quorum project onto it. **This file is the only place
setup steps live** — README, DEVELOPMENT, and OPERATOR link here instead of
repeating them.

Every step has an ID, and `make doctor` checks each one on the current machine
(read-only, no tokens). You're done when doctor reports **0 failed**. The
[last section](#keeping-this-runbook-true) says how the two stay in step.

```bash
make doctor                         # machine steps S1–S10
make doctor PROJECT=~/work/myproj   # plus project steps P1–P6 for one project
```

## Machine (once per machine)

### S1 — Toolchain

A C++20 compiler, CMake ≥ 3.20, make, git, python3.

```bash
# Linux / WSL2 (Ubuntu 24.04)
sudo apt-get install -y build-essential cmake git python3 ninja-build   # ninja optional

# macOS
xcode-select --install && brew install cmake
```

### S2 — Libraries

Development headers for libcurl, OpenSSL, and SQLite.

```bash
# Linux / WSL2
sudo apt-get install -y libcurl4-openssl-dev libssl-dev libsqlite3-dev

# macOS (curl comes with the SDK; CMake finds Homebrew's keg-only OpenSSL itself)
brew install openssl@3 sqlite
```

### S3 — Claude Code CLI

Every agent turn is a `claude -p` subprocess, so the CLI must be installed and
signed in.

```bash
curl -fsSL https://claude.ai/install.sh | bash
claude auth login
```

### S4 — Clone and build

The checkout can live anywhere: agents refer to its templates as `$QUORUM/…`,
which resolves to the checkout the running binary was built from.

```bash
git clone git@github.com:nonvis/quorum.git ~/nonvis/quorum
cd ~/nonvis/quorum
make build            # -> build/quorum_daemon; `quorum version` names the built sha
```

After every `git pull`, run `make build` again. Doctor warns when the build
doesn't match the checkout.

### S5 — Tests

```bash
make test                                          # C++ (ctest)
make web-install && (cd quorum-web && bun test)    # web (needs S8's bun)
```

### S6 — Install

```bash
make install
```

- Links `~/.local/bin/quorum` to `build/quorum_daemon`. `~/.local/bin` must be
  on `PATH` (Ubuntu's `~/.profile` adds it once the directory exists).
- Links into `~/.claude` the pieces Claude Code itself has to find: the skills
  `scripts/install-skills.sh --list` prints, and the autopilot agent
  `~/.claude/agents/supervisor.md`. They are **links into the checkout**, so
  `git pull` updates them. The knower lens skills are not installed; agents
  read them in place.
- `make uninstall` removes the CLI link and only those `~/.claude` links that
  point into this checkout.

### S7 — GitHub CLI

The historian's Tier-1 scan (`decisions-raw.json`) reads pull requests through
`gh`. Without a signed-in `gh`, `setup-knowers.sh` skips that scan entirely and
the historian has nothing to read (doctor P4 shows it missing); the recap scan
still runs on commits alone.

```bash
sudo apt-get install -y gh      # macOS: brew install gh
gh auth login
```

### S8 — Web dashboard (optional)

```bash
curl -fsSL https://bun.sh/install | bash       # macOS: brew install oven-sh/bun/bun
sudo apt-get install -y tmux                   # macOS: brew install tmux — web flight launch
make web-install
./scripts/web.sh start                         # UI http://localhost:3101, API :3100
```

The bun installer adds `~/.bun/bin` to `~/.bashrc`, which non-interactive
shells (Claude Code's own tool calls included) skip on Ubuntu. To give those
shells `bun` too, add `export PATH="$HOME/.bun/bin:$PATH"` to `~/.profile`.

### S9 — Docent (optional)

`quorum-own-agent/` is plain Python 3 (standard library only). Its index needs
SQLite's FTS5 in Python's `sqlite3`, which the stock Ubuntu and macOS Pythons
have. Nothing to install; doctor checks it.

### S10 — Live smoke (spends tokens)

```bash
make smoke
```

This runs one real `claude -p` round trip through the daemon on a throwaway
project (one cartographer pass, about $1, a few minutes) and records the Claude
Code version it passed on. The daemon parses the CLI's JSON reply, and a CLI
update once made it reject every healthy reply. **Run it on a new machine and
after every Claude Code update**; doctor warns when the installed CLI differs
from the last one that passed.

## Project (once per project per machine)

Git carries a project's `.quorum/config.yaml`, `agents/`, vault `CONTEXT.md`
files, and Tier-1 inputs. It does **not** carry `quorum.db` (conversations,
pending gates), the knowers' knowledge (`vaults/*/knowledge/`), or Claude Code's
transcripts, which `quorum spend` reads. A new machine starts without them.
Copy them from the old machine if you need the history; otherwise P5 rebuilds
the knowledge.

### P1 — Clone and scaffold

Clone the project. If it has no `.quorum/` yet, scaffold it from its root. Zero
tokens.

```bash
cd ~/work/myproj && quorum init
~/nonvis/quorum/scripts/setup-knowers.sh ~/work/myproj
```

### P2 — Portable paths

Tracked agent files store paths as `~/…`, `$QUORUM/…`, or project-relative.
Projects set up before that convention carry another machine's absolute paths,
and their agents run without their skills. Repair them from the project root,
then commit the yaml change in the project:

```bash
quorum agent relink --dry-run    # shows each rewrite
quorum agent relink
```

### P3 — Leader role skill

Projects scaffolded before 2026-09-05 have a `leader.yaml` with no role skill.
Add this line to `.quorum/agents/leader.yaml`:

```yaml
skill_file: ~/.claude/skills/quorum-roles/leader/SKILL.md
```

### P4 — Tier-1 knower inputs

The deterministic scans the knowers read (`layout.json`, `decisions-raw.json`,
`timeline-raw.json`). Zero tokens, and safe to re-run.

```bash
~/nonvis/quorum/scripts/setup-knowers.sh ~/work/myproj
```

### P5 — Knower knowledge (spends tokens)

```bash
quorum knower refresh --all --parallel --project ~/work/myproj
```

Four Tier-2 passes, 10–25 minutes. When it finishes, the daemon auto-commits
`.quorum/**` in the project, so have nothing else staged there.

### P6 — Register the project

If you drive Quorum from a notes vault (the `quorum` skill), add the project to
that skill's registry as a `~/` path so the same row works on every machine.

## Keeping this runbook true

- Setup steps live here and nowhere else. `test_setup_runbook` (run by
  `make test`) fails when this file's step IDs and `scripts/doctor.sh`'s `step`
  IDs differ, or when README, DEVELOPMENT, or OPERATOR grow their own
  `apt-get install` / `brew install` lines.
- A change to dependencies, the build, `make install`, the installed skill list
  (`SKILLS` in `scripts/install-skills.sh`), the path conventions, or what a
  project needs updates this file **and** `scripts/doctor.sh` in the same
  commit.
- When setting up a machine turns up a step this file lacks, fix the file (and
  add its doctor check) rather than keeping a side note.
