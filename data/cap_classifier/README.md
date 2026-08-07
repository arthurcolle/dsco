---
license: apache-2.0
pretty_name: DSCO Capability-Gate Agent Trajectories
size_categories:
- 1M<n<10M
task_categories:
- reinforcement-learning
- text-classification
tags:
- agent
- tool-use
- function-calling
- safety
- guardrail
- lethal-trifecta
- verifiers
- prime-rl
- atropos
configs:
- config_name: trajectories
  default: true
  data_files:
  - split: train
    path: trajectories/train.jsonl
  - split: validation
    path: trajectories/validation.jsonl
  - split: test
    path: trajectories/test.jsonl
- config_name: turns
  data_files:
  - split: train
    path: train_mt_1m.ndjson
---

# DSCO Capability-Gate Agent Trajectories

Scored, multi-turn agent **tool-calling trajectories** where a deterministic **capability
gate** is the verifier. Each session is an agent solving a task by calling tools; the gate
enforces a **lethal-trifecta** exfiltration policy (untrusted-content ingested + secrets
accessed + egress to an *external* destination ⇒ deny) on every call. Because taint
accumulates across turns, the *same* call is `allow` early in a session and `deny` after
the session has read a secret and ingested untrusted content — the temporal signal a
single-turn dataset cannot capture.

Format follows established agentic-dataset conventions: OpenAI/ShareGPT `messages` with
tool calls (cf. [Nous Hermes function-calling](https://huggingface.co/datasets/NousResearch/hermes-function-calling-v1)),
scored trajectories as the data atom (cf. [Nous Atropos](https://github.com/nousresearch/atropos)
`ScoredDataGroup`), a task + verifier/rubric pairing (cf. [Prime Intellect verifiers](https://github.com/PrimeIntellect-ai/verifiers)
/ [prime-rl](https://github.com/PrimeIntellect-ai/prime-rl)), and `reward`/`score`/
`verification_info`/`metadata` columns (cf. [Orchard](https://huggingface.co/datasets/microsoft/Orchard),
[AgentTrove](https://huggingface.co/datasets/open-thoughts/AgentTrove)).

## Configs

- **`trajectories`** (default) — one row per session, RL/SFT-ready.
- **`turns`** — the flat per-turn NDJSON (one row per tool call) for training the guard
  classifier directly. Single-turn variant: `train.ndjson`.

## `trajectories` schema

| field | type | meaning |
|---|---|---|
| `id` | string | `cap-mt-{session:07d}` |
| `messages` | list | OpenAI chat: `system`, then per turn an `assistant` with `tool_calls` and a `tool` response. Each assistant message carries `reward` (+1 allow / −1 deny) and a `gate` object (decision, category, risk, egress, tainted, private, caps). |
| `tools` | list | Available tool schemas (`type:function`) used in the session. |
| `reward` | float | **1.0** iff the agent completed the session with **no** gate-denied action, else **0.0**. |
| `score` | float | Fraction of turns allowed, in [0,1]. |
| `num_turns` | int | Turns in the session. |
| `verification_info` | string(JSON) | `{verifier:"dsco-capability-gate", policy:"lethal-trifecta", deterministic:true, denied_turns:N}`. |
| `metadata` | string(JSON) | `{per_turn:[…], tier}` — the full per-turn feature/label vectors (variable-shape side data, per convention). |

## `turns` schema (per tool call)

`session, turn, tool, tier, caps, egress ∈ {none,local,trusted,lan,external,opaque}, dest,
tainted, private, input_secrets, shell_writes, spawn, exec, net, write, read, control,
risk ∈ [0,100], decision ∈ {allow,deny}, category (17-class), input`.

## Labels & verifier

The label generator is the real gate (`dsco-cli/src/capability.c`), so labels match the
deployed policy exactly. `reward` is a verifiable RL signal for training safer agents;
`decision`/`category` are supervised targets for training the guard classifier. The gate
is deterministic given (tool, input, tier, accumulated taint, operator grants).

## Load

```python
from datasets import load_dataset
traj = load_dataset("dsco/capability-gate-trajectories", "trajectories")   # RL/SFT
turns = load_dataset("dsco/capability-gate-trajectories", "turns")         # guard classifier
```

## Provenance

Tool vocabulary includes the live Tool Management API integrations
(`tools.distributed.systems`: distributed-memory ingress, distributed-publish egress)
alongside synthetic shells. Generators: `tools/cap_dataset_gen.c` (single-turn),
`tools/cap_dataset_gen_mt.c` (multi-turn), `tools/cap_to_trajectories.py` (reshape).

## Splits

train / validation / test = 90 / 5 / 5 by stable session hash; see `trajectories/stats.json`.
