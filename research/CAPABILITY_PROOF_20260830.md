# DSCO Capability Proof — tools.distributed.systems

**Date:** 2026-08-30 (all facts re-verified on disk/binary on this date unless noted)
**Repo:** `/Users/arthurcolle/dsco-emergency/dsco-cli`, branch `fix/ci-release-readiness` @ `0bd4cb7`
**Binary under test:** `./dsco` v1.0.2, built 2026-08-27T02:26:16Z at ref `4752898`
**Purpose:** evidence-cited capability proof supporting the GTM vector (tools.distributed.systems developer access) and the $300K / 1-year revenue objective. Written per DSCO epistemic law: every claim carries an evidence class; observed facts and intended designs are never conflated.

**Evidence classes used:**
- **[B]** — computed from the built binary (`./dsco --version`, `./dsco runtime status`)
- **[D]** — computed from disk/source at HEAD `0bd4cb7`
- **[R]** — live runtime observation (this session, commands reproduced in §4)
- **[A]** — verified in a throwaway build copy (`/tmp/dsco-gate-verify`) with two mechanical HEAD fixes applied; repo tree untouched
- **[H]** — historical artifact (committed swarm reports); reproducible but not re-executed here

---

## 1. Executive summary

DSCO is a local-first agentic runtime in pure C, large enough and hardened enough to be evaluated as production infrastructure rather than a demo. On 2026-08-30 a multi-worker swarm audited and reconciled the platform's own calibration with zero inter-worker contradictions — which is itself the demonstrated orchestration capability this document sells. The three sellable, verified claims:

1. **Scale with provenance:** 205,172 hand-written C/header LOC across 304 files (plus 213,226 LOC of generated embedded-data blobs) — every number computed from disk, with the generated-vs-hand-written distinction audited so no one can accuse the count of padding [D].
2. **Governed capability plane, proven end-to-end:** 277 built-in tools behind a capability gate whose hard denials (control-plane, untrusted-tier egress, lethal trifecta) were verified by a 6/6-claim test harness and by live JSON-RPC denial exchanges [B][R][A].
3. **Demonstrated distributed orchestration:** a 2026-08-30 swarm of 5 dispatched workers + coordinator that produced 5 independent reports, recovered one worker via re-dispatch, and resolved a cross-worker methodology conflict without contradiction [H][R].

The revenue tie is direct: each paid evaluation of tools.distributed.systems starts with the §4 demo script — a stranger can reproduce every claim in ~10 minutes on a Mac without touching our data, credentials, or network. Capability proof *is* the GTM artifact; it converts trust-building (normally weeks of sales cycles) into a self-service reproducible test.

---

## 2. Verified platform facts

Every row below was re-verified 2026-08-30. Nothing in this table is recalled from memory.

| # | Fact | Value | Evidence class | How verified |
|---|---|---|---|---|
| 2.1 | Version | `dsco v1.0.2 (built 2026-08-27T02:26:16Z, 4752898)` | [B] | `./dsco --version` |
| 2.2 | Hand-written C/header LOC | **205,172 across 304 files** (146 `.c` + 147 `.h`, excludes `src/generated/`) | [D] | single-pass `find … -exec cat + \| wc -l`, recomputed 2026-08-30 |
| 2.3 | Generated embedded blobs | 213,226 LOC / 3 files (`src/generated/`); 418,398 total | [D] | `01-loc-audit.md` + direct recomputation; breakdown: tool embeddings 124,255; env index JSON 88,937; secrets stub 34 |
| 2.4 | Hand-written growth rate | +1,409 LOC (+0.7%) in ~6 weeks vs 2026-07-14 baseline (203,763) | [D] | `01-loc-audit.md` — refutes any "2× growth" inflation |
| 2.5 | Built-in tools | **277 builtin / 44 core** exposed by default | [B] | `./dsco runtime status` → `"tools":{"builtin":277,"core":44,...}` |
| 2.6 | Capability-gated execution | Gate classifies fs/net/exec/secrets/untrusted/control; hard denials verified: control-plane blocked by default; untrusted tier blocks `http_request` (JSON-RPC error `-32000`); lethal-trifecta egress after secrets+untrusted blocked, with operator override `DSCO_ALLOW_EXFIL=1` | [R][A] | live denial exchange (§4.4) + `make test-gate-claims` → **ALL GATE CLAIMS VERIFIED (6/6)** |
| 2.7 | MCP server mode | Full JSON-RPC `initialize` → `protocolVersion 2025-06-18` → `tools/list` serving typed tool schemas over stdio | [R] | live exchange (§4.3); server: `dsco v2.1` |
| 2.8 | Bounded tool disclosure | `killswitch` (control-plane) is **not served at all** under governed posture — disclosure discipline, not just execution blocking | [R] | live `tools/call` → `"tool not exposed by enabled toolsets"`; confirmed by gate test claim `unexposed-tool-not-served` [A] |
| 2.9 | Multi-provider orchestration | Claude Code (authenticated via env) + ChatGPT Codex (authenticated via `~/.codex/auth.json`) + llamacpp backend (`Qwen3.6-27B-Q6_K`); `--provider-fabric` (race/spawn/collect modes, replica caps, timeout control), `-O` orchestrator routing to specialist workers | [B][R] | `./dsco status`; `./dsco --help` flags |
| 2.10 | Branch substance | `fix/ci-release-readiness` @ `0bd4cb7` is 66 commits ahead / 0 behind the prior snapshot: +104 files, +38,068/−2,334, incl. Guardian covenant/tenure/ledger modules, swarm lane admission control, worker retry/backoff, durable-record secret scrubbing | [D] | `05-branch-diff-audit.md` + `git log` |
| 2.11 | Governance telemetry | Gate call counters with per-call latency exposed at runtime (`gate_calls`, `gate_ms_total`, `gate_ms_avg`) | [B] | `./dsco runtime status` JSON |
| 2.12 | Posture control | Governance model selectable `none\|minimal\|audit\|standard\|paranoid`; trust tiers `trusted\|untrusted`; `--sandboxed`, `--autonomous`, `--systems-agent` postures | [B][R] | `./dsco --help`; denial demos required explicit posture env (see §4.4 note) |

**Identity-doc drift note (found during verification):** the 2026-08-30 calibration pass recorded the binary build ref as `56be6a8` (`impl-calibration.md`), but the binary's own stamp reads `4752898` — two commits *earlier* than `56be6a8`. This document trusts the binary stamp [B] over the doc. Drift logged for repair; it does not affect any capability claim above.

---

## 3. Live proof-of-orchestration: the 2026-08-30 swarm run

This section describes a real run executed by the dsco runtime on 2026-08-30, 20:26–20:50 local time. It is offered as a demonstrated distributed-orchestration capability, with all claims tied to committed artifacts in `.workspace/harness-parity/swarm-20260827/`.

**Topology and workload.** The coordinator decomposed one objective — full platform recalibration plus Wave B scoping — into 5 independent map tasks plus direct coordinator computation:

| Worker | Report | Task | Outcome |
|---|---|---|---|
| W1 | `01-loc-audit.md` | LOC/methodology audit | Definitive single-pass recount; root-caused the 418K-vs-203K "drift" to generated blobs |
| W2 | `02-durable-execution-design.md` | Durable execution design (W1–W6) | Implementation-ready; discovered `dsco runs`/`--resume` already landed, cutting scope |
| W3 | `03-eval-harness-design.md` | Eval harness design | 5 seeded cases; 3 runnable provider-free |
| W4 | `04-observability-design.md` | Observability/cost receipts design | Cost receipts scoped to smallest viable change |
| W5 | `05-branch-diff-audit.md` | Branch diff audit | 66-commit ancestry + working-tree risk surfaced |

**Fault handling.** One worker was **re-dispatched with a tightened prompt** after its first output was insufficient — recorded in `00-SYNTHESIS.md` ("Workers: 5 map tasks (1 re-dispatched, tightened)"). This is observed adaptive retry: the coordinator inspected the failure, re-scoped, and re-ran rather than accepting a weak result.

**Conflict reconciliation.** Workers W1 and the coordinator initially disagreed on LOC methodology (chunked `xargs wc -l` vs single-pass). The conflict was resolved on evidence — single-pass won — and the resolution is documented in `01-loc-audit.md` §"Definitive numbers". Final synthesis (`00-SYNTHESIS.md` §4) records: **"No inter-worker contradictions."**

**Verified integration.** The coordinator did not just collect reports; it cross-checked them (audit numbers confirmed exactly against disk before any identity doc was edited, per `impl-calibration.md` "Pre-edit verification"), then executed the reconciliation edits across `AGENTS.md` + three workspace identity docs with post-edit stale-token grep verification. Total wall time from first report (20:26) to final synthesis log (20:50): **~24 minutes**.

**Honest scoping of this claim.** What is demonstrated: map/fan-out decomposition, worker retry via re-dispatch, cross-worker conflict detection and evidence-based resolution, zero contradictions, coordinator-side integration and verification. What is *not* separately evidenced here: the precise timeout parameters of the retry path and per-worker model attribution (not logged in the artifacts). Distributed-orchestration *primitives* (retry/backoff, budget checkpoints, heartbeats, retention of last-good output) are separately present in source at HEAD (`orchestrator.c`, `swarm.c` — [D], per `05-branch-diff-audit.md` §"Runtime-specific delta") but their W-journal durability is design-stage, not shipped (see §5).

---

## 4. Reproducible demo script (evaluator-run, no provider spend, no network egress)

All commands below were executed on 2026-08-30 and their observed outputs are quoted. Working directory: the repo root. Requirements: macOS with clang, GNU make; no API keys needed for any step.

### 4.1 Build and smoke

```bash
make -j8
./dsco --help
```

> **⚠ Known HEAD defect (2026-08-30):** a cold `make` at `0bd4cb7` currently **fails** with two mechanical, fully-diagnosed errors (see §5.1). Either use the prebuilt binary already in the repo root for steps 4.2–4.4, or apply the two fixes (verified: build then completes and links). This is stated up front because an evaluator must never discover it unlisted.

Expected: `dsco v1.0.2 — local-first agentic C runtime (streaming + prompt caching)` and the full subcommand surface.

### 4.2 Platform facts from the binary

```bash
./dsco --version        # → dsco v1.0.2 (built 2026-08-27T02:26:16Z, 4752898)
./dsco runtime status   # JSON: version, tools {builtin:277, core:44}, governance telemetry, workspace counts
```

### 4.3 MCP server mode (JSON-RPC over stdio)

```bash
printf '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"eval","version":"0"}}}\n{"jsonrpc":"2.0","id":2,"method":"tools/list"}\n' \
  | ./dsco mcp serve
```

Observed response to line 1:
```json
{"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-06-18","capabilities":{"tools":{"listChanged":false}},"serverInfo":{"name":"dsco","version":"2.1"}}}
```
Line 2 streams the typed tool schemas (`write_file`, `read_file`, `edit_file`, `grep_files`, `bash`, …). Any MCP client can drive this server as-is.

### 4.4 Capability-gate denial demo

Run with the governed posture forced explicitly (note: if your shell exports `DSCO_GOV_MODEL=none` / systems-agent posture, the default governed denials won't fire — the explicit env below is not pedantry, it's required):

```bash
env -u DSCO_ALLOW_NET -u DSCO_ALLOW_RUN \
    DSCO_GOV_MODEL=standard DSCO_APPROVAL_MODE=never DSCO_TRUST_TIER=untrusted \
  bash -c 'printf {"jsonrpc":"2.0","id":1,"method":"initialize",...}\n
           {"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"http_request","arguments":{"url":"http://example.com"}}}\n' \
  | ./dsco mcp serve
```

Observed denial (executed live 2026-08-30):
```json
{"jsonrpc":"2.0","id":2,"error":{"code":-32000,"message":"tool 'http_request' is blocked in untrusted tier"}}
```

Second denial, control-plane class — under the same governed posture, calling `killswitch {"action":"trigger"}` returns:
```json
{"jsonrpc":"2.0","id":2,"error":{"code":-32602,"message":"tool not exposed by enabled toolsets"}}
```
i.e. the control-plane tool is withheld from disclosure entirely (bounded disclosure), not merely blocked at execution.

### 4.5 The authoritative gate test (after §5.1 fixes)

```bash
make test-gate-claims
```

Executed 2026-08-30 in a fix-verified copy; full transcript claim set:

```text
ok:   control-denied-by-default (killswitch trigger)
ok:   control-grant-accepted (killswitch status (read-only exempt))
ok:   trifecta-denied-exfil0 (secrets->egress under DSCO_ALLOW_EXFIL=0)
ok:   trifecta-override-exfil1 (operator override honored)
ok:   untrusted-tier-net-denied (http_request at untrusted tier)
ok:   unexposed-tool-not-served (toolset disclosure discipline)

ALL GATE CLAIMS VERIFIED (6/6)
```

The test harness drives `dsco mcp serve` over JSON-RPC with no LLM or API key — an evaluator verifies the entire governance story without spending a cent or touching our infrastructure.

---

## 5. Honest limitations (observed vs intended)

Stated per DSCO epistemic law. Nothing in §1–§4 depends on anything in this section being false; conversely, nothing in §1–§4 should be quoted without the caveats here.

### 5.1 HEAD `0bd4cb7` does not compile (verified, two mechanical defects)

A clean `make` at HEAD fails. Root causes, verified line-by-line:

1. **`include/orchestrator.h:33`** — `void orch_budget_checkpoint(char *result, size_t cap);` uses `size_t` with only `#include <stdbool.h>` present. Fix: add `#include <stddef.h>`. Verified: with this line, `src/main.c` passes `cc -fsyntax-only`.
2. **`src/orchestrator.c:183,187`** — the T04/T05 seam block (`orch_http_transient`, `orch_budget_checkpoint` definitions) was inserted **inside the body of `run_worker_task`**, producing "function definition is not allowed here". Fix: relocate the 16-line block to after `run_worker_task`'s closing brace. Verified: with both fixes applied in a throwaway copy, `make -j8` completes and links both `dsco` and `spine-dsco-slim`, and `make test-gate-claims` passes 6/6 [A].

This is the same working-tree seam flagged as pre-existing risk in `05-branch-diff-audit.md`, now committed. The shipped binary (`4752898`) predates both defects and is fully functional — but "the binary works" and "the tip of the branch builds" are different claims, and only the first is currently true. My write scope for this document was `research/CAPABILITY_PROOF_20260830.md` only, so the two-line repair is documented here rather than applied. Fix it before any external demo that includes a cold build.

### 5.2 Binary vs HEAD skew

The tested binary is 3 commits behind HEAD (`4752898` vs `0bd4cb7`): guardian foundation → guardian T12/T13 → scale engine T21-T23 v1. Capabilities present only in those commits (lane admission semaphores, tenure election) are source-inspected [D], not runtime-observed. The calibration doc `impl-calibration.md` also misattributes the binary build ref as `56be6a8`; the binary stamp says `4752898` (see §2 drift note).

### 5.3 What is design vs shipped in Wave B

- **Shipped & runtime-verified:** capability gate + all 6 hard-denial claims; MCP server mode; multi-provider status; `dsco runs` / `--resume` CLI seams; CRC32-journaled Chronicle WAL seam (`chronicle_journal_append` — source-verified).
- **Design-complete, not shipped:** the full durable-execution W1–W6 plan, `kill -9` replay guarantee, cost receipts persistence (`~/.dsco/usage/`), the eval harness cases (only designs exist in `02`/`03`/`04-*design.md`).
- **Not yet evidenced:** clean CI run on this branch (`05-branch-diff-audit.md` explicitly notes the audit "does not establish that the current branch builds cleanly or that CI passes" — since corroborated by §5.1); durable-agent recovery under SIGKILL; the eval harness returning pass/fail on seeded cases.

### 5.4 Swarm-run evidence boundary

The §3 run is evidenced by committed artifacts and file timestamps, and its reconciliation outcomes were re-verified against disk. It was a single-session, single-host orchestration with LLM-backed workers. Retry is evidenced as re-dispatch-with-tightened-scope; timeout/backoff parameters were not logged. It demonstrates orchestration discipline at swarm-of-5 scale on one coordinator — not multi-host fleet operation, which exists as capability class (`bridge/fanout`, mesh) but was not exercised in this run.

### 5.5 Counts are telemetry, not identity

277 tools, 100,132 skills, 72 doctrines, etc. are point-in-time runtime/disk observations. They are regenerated from source and drift by design; the proof of the platform is the reproducible §4 script, not any single number.

---

## 6. Provenance

- Inputs read first per directive: `00-SYNTHESIS.md`, `01-loc-audit.md`, `05-branch-diff-audit.md` (all in `.workspace/harness-parity/swarm-20260827/`).
- All §2 rows recomputed 2026-08-30 from binary, disk, and live runtime, not copied from earlier session logs.
- Throwaway verification copy: `/tmp/dsco-gate-verify` (src/include/gsl/vendor/data/scripts/build copied; two mechanical fixes applied there only). **The repo tree received zero source modifications and nothing was committed** — this document and the 5-line summary in `impl-gtm.md` are the only writes.
- Author: dsco runtime coordinator session, 2026-08-30.
