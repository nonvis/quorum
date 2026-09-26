# /quorum-setup — bring a project (or this machine) up to SETUP.md

`SETUP.md` at the repo root is the single source of truth for setting up
Quorum. This command walks it; it does not restate it. If a step here and
`SETUP.md` ever disagree, `SETUP.md` wins — fix this file.

## Steps

1. Ask which project directory to set up (default: none — machine only).
2. Run `scripts/doctor.sh` (with `--project <dir>` when a project was named) and
   show the result.
3. For each line that is not `ok`, read that step in `SETUP.md` (the ID — `S4`,
   `P2`, … — is its heading) and offer to run its commands, one step at a time,
   in ID order. Ask before anything that:
   - installs system packages (`sudo`) or signs in (`claude auth login`,
     `gh auth login`) — hand those to the user as `! <command>`;
   - spends tokens (`make smoke`, `quorum knower refresh`) — say the cost first;
   - rewrites tracked files in the project (`quorum agent relink`, the P3 leader
     line) — show the dry run / diff first; committing is the user's call.
4. Re-run doctor and report what is still not `ok`.

## Choosing agents beyond the default roster

`quorum init` already creates the leader, the four knowers, the thinker, and a
language doer per detected marker. Add others only when the user asks:

```bash
quorum agent create --role doer --name <name> --target-dir . --no-ai
quorum agent create --role evaluator --name <name> --no-ai
~/nonvis/quorum/scripts/setup-advisor.sh <project>   # spends one LLM pass
```

## Rules

- Never overwrite an existing `CONTEXT.md` or agent yaml by hand; use
  `quorum agent modify` / `quorum agent relink`.
- Anything the walk turns up that `SETUP.md` lacks: add it to `SETUP.md` and a
  matching `step` in `scripts/doctor.sh` (ctest `test_setup_runbook` enforces
  the pairing).
