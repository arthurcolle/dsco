# Fisher/EWC Retention Clock Spec

Status: spec-only for this repository. `include/memory_tier.h` defines
`memory_entry_t`, not the 64-byte graph `Node` substrate referenced by the
June 23 design. No `propose_mutation` protocol is present in this tree. The
field layout below is therefore the binding layout for the GraphSub Node
substrate when it lands here or is implemented in GraphSub.

## 1. Node Reserved Bytes

Target record: 64-byte Node, little-endian scalar layout.

| Offset | Size | Field | Purpose |
| --- | ---: | --- | --- |
| 0x00 | 8 | `node_id` | Stable node identifier. |
| 0x08 | 4 | `flags` | Active, tombstone, classification, dirty bits. |
| 0x0c | 2 | `tier` | PLASMA, working, episodic, semantic, ARCTIC. |
| 0x0e | 2 | `heat_q16` | Quantized thermal heat score. |
| 0x10 | 8 | `created_at_ms` | Creation time. |
| 0x18 | 8 | `last_touch_ms` | Last access or distillation touch. |
| 0x20 | 4 | `access_count` | Saturating access counter. |
| 0x24 | 2 | `fisher_q16` | Quantized Fisher importance score. |
| 0x26 | 2 | `ewc_flags` | Fisher valid, anchor valid, distill pending. |
| 0x28 | 8 | `anchor_weight_ref` | Pointer/handle to consolidated anchor weights. |
| 0x30 | 8 | `adjacency_ref` | Graph edge/block reference. |
| 0x38 | 8 | `payload_ref` | Payload/value reference. |

`fisher_q16` stores `round(clamp(F_i, 0, F_max) / F_max * 65535)`.
`anchor_weight_ref` is zero when no consolidated anchor exists. The anchor
object stores immutable weights `theta*_i` and optional scale metadata.

## 2. Parametric Mutation

New mutation kind: `PARAMETRIC_WEIGHT_UPDATE`.

Required payload:

- `target_node_id`
- `gradient_stream_id`
- `theta_delta_ref`
- `optimizer_state_ref`
- `trajectory_cert_ref`
- `fisher_update_q16`
- `anchor_weight_ref`

Validation order:

1. Parse model output tolerantly into a mutation candidate.
2. Canonicalize to the rigid mutation schema.
3. Require trajectory-level certification over the entire gradient stream.
4. Reject point-wise SGM-only certification. Piecewise gates are bypassable and
   cannot authorize a parametric update.
5. Verify the candidate decreases the extended Lyapunov function or is explicitly
   tagged as a reversible probe with no tier transition.
6. Apply through `propose_mutation`; never mutate Node weights directly.

## 3. Lyapunov Function

Base lifecycle energy is extended with the EWC penalty:

```text
V_total = V_thermal + V_graph + V_budget
        + (lambda_ewc / 2) * sum_i F_i * (theta_i - theta*_i)^2
```

Acceptance condition:

```text
Delta V_total <= epsilon
```

For destructive forgetting, `epsilon` must be zero or negative. Positive
epsilon is only allowed for reversible probes that do not change durable tier,
anchor references, or ARCTIC replay eligibility.

## 4. Tier Transitions

Thermal lifecycle transitions are redefined as gradient-distillation events:

- PLASMA to working: admit transient gradient evidence, no anchor required.
- Working to episodic: distill repeated gradients into a candidate Fisher score.
- Episodic to semantic: require trajectory certification and anchor creation.
- Semantic to ARCTIC: freeze anchor weights and retain replay exemplar metadata.
- ARCTIC to active recall: load exemplar plus anchor; do not overwrite anchor
  unless a new certified trajectory supersedes it with higher incarnation.

Heat can request a transition, but Fisher importance can veto forgetting.
Forgetting a low-heat node with high `fisher_q16` is illegal until a replacement
anchor and replay exemplar are certified.

## 5. Replay Mixture

Replay sampling must include ARCTIC-tier exemplars:

```text
batch = p_live * live_samples
      + p_episodic * episodic_samples
      + p_arctic * arctic_exemplars
```

`p_arctic` has a nonzero floor whenever any active parameter has
`fisher_q16 > fisher_replay_gate`. The sampler weights ARCTIC exemplars by
Fisher score first, then by recency within equal Fisher buckets.

## Repository Decision

This repo does not currently contain the 64-byte Node substrate or the
`propose_mutation` protocol. No compile-time Fisher field is implemented here.
The implementation trigger is the arrival of that substrate in this repo; until
then, this spec is the handoff contract for GraphSub.
