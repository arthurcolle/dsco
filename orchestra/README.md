# Self-Play Capabilities Orchestra

A recursive capability-compounding engine. Not a benchmark, not an audit — a
generative loop where a swarm invents its own challenges, competes to solve
them, judges the results, and ratchets difficulty toward the frontier.

## The loop (one "movement")

```
                ┌──────────────────────────────────────────┐
                │            CONDUCTOR (coordinator)         │
                │  holds the score, sets tempo + difficulty  │
                └───────────────┬────────────────────────────┘
       ┌────────────────────────┼─────────────────────────────┐
       ▼                        ▼                              ▼
  ┌─────────┐            ┌──────────────┐              ┌───────────────┐
  │ COMPOSER│  proposes  │  SOLOISTS    │  compete to  │    CRITIC     │ scores
  │ (setter)│──challenge─▶│ (N solvers,  │──solutions──▶│  (judge +     │─┐
  │         │            │  diverse mdl)│              │   rubric)     │ │
  └─────────┘            └──────────────┘              └───────────────┘ │
       ▲                                                                  │
       │                     LEDGER (durable)                            │
       └──────────────  difficulty ratchet ◀── credit assignment ◀───────┘
```

- **Composer** generates a challenge calibrated to the current frontier score.
  It is rewarded when a challenge is *hard but solvable* (some soloists pass,
  some fail) — self-play pressure toward the edge of competence.
- **Soloists** are independent model instances (diverse providers) that each
  attempt the challenge. They compete on a rubric.
- **Critic** scores every solution against an explicit rubric, picks a winner,
  and emits credit signals.
- **Conductor** updates the frontier difficulty from the pass-rate, records the
  movement to the ledger, and starts the next movement harder or easier.

## Why this compounds capability

1. Difficulty auto-calibrates to ~50% pass rate — the maximal-learning zone.
2. Diverse soloists (different models) means winning strategies transfer.
3. The ledger accumulates solved challenges as a growing skill corpus.
4. Composer + Critic co-evolve with soloists (generative adversarial pressure).

## Files

- `orchestra.sh`      — driver: runs N movements, persists the ledger
- `roles/`            — role prompts (composer / soloist / critic / conductor)
- `ledger/`           — per-movement JSONL: challenge, solutions, scores, winner
- `frontier.json`     — current difficulty + rolling pass-rate
- `corpus/`           — solved challenges promoted to reusable skills

## Run

```bash
# dry-run (prints the plan, no model calls)
./orchestra/orchestra.sh --movements 3 --soloists 4

# live (needs provider lanes)
DSCO_ORCHESTRA_LLM=1 ./orchestra/orchestra.sh --movements 3 --soloists 4 \
    --domain "systems programming + agent orchestration"
```
