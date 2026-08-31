# Governance Posture — Deliberate, Not Ambient

**Status:** ratified operating posture, 2026-08-31 (principal: Arthur Colle)
**Authority:** CONSTITUTION.md capability law; `src/capability.c` gate; `src/main.c --systems-agent`

## Posture matrix

| Surface | Governance | How it is entered |
|---|---|---|
| Normal CLI / agent sessions | **governed** (`standard` model, `trusted` tier) | default — no action |
| Make-spawned processes (tests, evals, verification) | **always governed** — overrides stripped by `unexport` guard at top of Makefile | automatic |
| Ungoverned control arm (systems-agent) | **none** — full gate bypass, unbounded permissions | explicit `--systems-agent` or `--gov-model none` on the binary invocation; loud red banner printed |

## The rule

Governance overrides (`DSCO_GOV_BYPASS`, `DSCO_GOV_MODEL=none`, `DSCO_ALLOW_{READ,WRITE,NET,RUN,SECRETS,CONTROL,EXFIL}`) are **never ambient environment inheritance**. They enter a process only through:

1. an explicit flag on the `dsco` invocation (`--systems-agent`, `--gov-model none`, or an explicit `DSCO_ALLOW_*` you set yourself for that one command); or
2. `make test`-style override *within* a verification harness (e.g. gate-claims V2 proves the documented `DSCO_ALLOW_CONTROL=1` override works — by setting it on that single subprocess).

## Why the guard exists

A `--systems-agent` parent passes its bypass environment down through fork/exec to every child — including eval and verification workers whose entire purpose is to measure the gate. An ambient override silently inverts such measurements (the 2026-08-31 false gate-failure: 3/3 + 6/6 clean, failures only under inherited bypass). The Makefile `unexport` guard makes all build/test/eval surfaces measure governed behavior regardless of what launched them.

## Provenance

- Finding: eval worker inherited `DSCO_GOV_BYPASS=1`, `DSCO_GOV_MODEL=none`, `DSCO_ALLOW_EXFIL=1`, `DSCO_ALLOW_CONTROL=1` from its systems-agent parent; gate code verified sound under clean env.
- Remedy: `unexport` block in `Makefile` (this PR) + this document.
- Residual: `--systems-agent` sessions still legitimately propagate the arm to their *own* spawned workers (that is its purpose); anything that must measure the gate must be launched via `make` or `env -u` its overrides explicitly.
