# 2048 B200 Cluster Runbook (3-day target)

Status: draft v1, single-node verticals verified, multi-node fan-out via SSH driver.
Branch: working line @ swarm-groups commit (256 groups x 64 governed lanes = 16K logical lanes/host ceiling).

## Verified capacity model (single host)

| Knob | Default | Range | Mechanism |
|---|---|---|---|
| `DSCO_SWARM_MAX_CHILDREN` | 64 | 1–64 hard | fail-closed spawn guards, 6 sites audited |
| `DSCO_SWARM_MAX_GROUPS` | 16 | 1–256 | structural; widened this commit |
| Logical lanes | 1024 legacy | **16384** | 256 groups x 64 lanes |

Bitset/ring/poll arrays stay correctly sized because *lane depth* is unchanged; scale rides the group axis. The full 2048-route catalog refactor (`proposals/swarm-scale-2048`, archived) remains the post-test North Star — do NOT attempt it before Thursday's run.

## Day-by-day

### Today (build + mock-proof)
- [x] group axis widened; suite green required before EOD
- [ ] `./dsco swarm` with 65/128/256-group ladders using `--worker 'echo {prompt}'`-style null lanes (no provider cost)
- [ ] capture baseline: scripts/dsco_perf_rollout.py + perf_diagram_suite.py numbers at 8/32/64 children x 1/16/64/256 groups
- [ ] soak: 10-min 64-child run, watch RSS + fd count (lsof), assert no zombie accumulation

### Day 2 (real lanes + knobs)
- [ ] provider fabric soak: max cheap lanes (e.g. 64x gpt-5.4-mini pattern) through swimlanes, budget-capped
- [ ] backpressure drill: kill mid-flight workers, verify completion-ring accounting and cancellation propagation
- [ ] OpenRouter ingestion: start route-catalog adapter only if time allows (P2, not blocking)

### Day 3 (cluster dry-run)
- [ ] pick driver: `fleet spawn` IF repaired (currently malformed ~/.dsco/fleet/hosts) else plain ssh+pssh loop over node list
- [ ] per-node: pull branch, `make -j$(nproc) dsco`, launch identical mock-lane ladder
- [ ] aggregate telemetry: each node emits per-second jsonl (started/running/completed/failed/throttled); concatenate
- [ ] acceptance: >=2048 logical lanes COMPLETED across cluster in <30min wall, zero unaccounted lanes

## Failure budget (pre-agreed degrades)
- fd exhaustion -> raise ulimit -n to 65535 on all nodes pre-run
- zombie buildup between ladders -> pkill pattern sweep script between runs
- github CI red does NOT block cluster day (it tracks hygiene gates, orthogonal to runtime scaling)

## Failure taxonomy (T10) — every lane ends in exactly one of these
| class | meaning | retry? | ledger entry |
|---|---|---|---|
| ok | natural end_turn with output | n/a | completed |
| budget_checkpoint | emitted-at-turn-limit (tagged) | no; forced summary | completed(checkpoint) |
| transient | HTTP 429/5xx/408/0 after retries exhausted | auto: 4x exp backoff (DSCO_WORKER_RETRIES) | failed(transient) |
| exhausted | context/token window died mid-run | no | failed(exhausted) |
| cancelled | veto/signal from Guardian or operator | by order | cancelled |
| throttled | semaphore/backpressure deferral | yes, parked | throttled |
| policy | hard gate (region/credential/tool req) | never | rejected(policy) |
Invariant at reduce time: launched == completed+failed+cancelled+throttled. Anything else = unaccounted = run invalid.
