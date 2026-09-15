# Autonomy prompt sweep (1,500 unique prompts)

Deterministic corpus-level review of task wording that informs dsco's runtime autonomy contract.
Prompt bodies are intentionally omitted from this artifact; use the script's `--samples` flag for local inspection.

## Corpus

| Source | Available unique | Selected |
|---|---:|---:|
| `prompt_pool_seed` | 1352 | 1000 |
| `runtime_news` | 934 | 300 |
| `low_level_eval` | 140 | 140 |
| `curated_repo_tasks` | 102 | 60 |
| **Total** |  | **1500** |

Corpus SHA-256: `9fb0a17411656c8fd88b96bd1c5875bbd2f13beed2e1d0d728afa9e8bbbe9064`

Runtime news cache: `/Users/arthurcolle/.dsco/prompt_pool.jsonl` (only records marked `src=news`; public headline-derived prompts).

## Language signals

| Signal | Prompts | Share |
|---|---:|---:|
| `imperative_start` | 873 | 58.2% |
| `concrete_deliverable` | 373 | 24.9% |
| `tool_or_execution` | 109 | 7.3% |
| `verification_evidence` | 206 | 13.7% |
| `completion_terminal` | 2 | 0.1% |
| `continuation` | 0 | 0.0% |
| `recovery` | 88 | 5.9% |
| `authority_boundary` | 6 | 0.4% |
| `parallel_delegation` | 6 | 0.4% |
| `permission_seeking` | 72 | 4.8% |
| `advisory_only` | 5 | 0.3% |
| `multi_stage_control` | 1 | 0.1% |
| `verified_terminal_state` | 0 | 0.0% |

## Prompt-engineering conclusions

- Task prompts commonly specify an action or artifact, but rarely define the agent's continuation, recovery, authority, and completion loop together.
- Verification language is materially more common than a verified terminal-state contract; the system prompt must join execution, evidence, and stopping criteria explicitly.
- Permission-seeking language is not the main corpus problem. The failure mode is an underspecified control policy: models can still narrate, defer, or stop after a partial result.
- Full and cheap modes therefore need one shared autonomy contract with an action default, bounded persistence, runtime-evidenced blockers, and exact authority limits.
- The contract should prohibit speculative tool denial while preserving actual capability gates and approvals as authoritative runtime boundaries.

## Reproduce

```sh
python3 scripts/review_autonomy_prompts.py --report reports/AUTONOMY_PROMPT_SWEEP_1500.md
```
