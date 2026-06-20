# MetaConstruct DSL

`StartOfLoopConstruct` accepts a bounded construct program that can keep an
agent loop alive after a model turn would otherwise be considered complete.
The program is construct-local: it can modify loop control, ontology state,
graph state, reward dynamics, learning signals, refinement rules, and bounded
schema rewrite rules without escaping the live loop stack.

## Control

```text
max_iterations = 5
max_turns = 80
continue when iteration < max_iterations and model_done
break when turn >= 40 or uncertainty < 0.1
prompt = "Continue the active construct and re-evaluate state."
```

Available loop variables include `iteration`, `next_iteration`, `remaining`,
`max_iterations`, `max_turns`, `turn`, `depth`, `model_done`, `has_followup`,
`override_done`, `override_max_turns`, and `recursive`.

## Meta/OORL State

```text
define(sensor, state_object)
object planner as policy_object
goal synthesize weight 0.8
task inspect priority 2
belief uncertainty = 0.7
infer posterior from evidence
learn rate = 0.25
policy = adaptive
decide greedy
```

Expressions can inspect `meta_count`, `definition_count`, `goal_count`,
`task_count`, `belief_count`, `object_count`, `dyad_count`,
`reward_object_count`, `refinements_applied`, and `rewrites_applied`.

## Mutable Ontology Graph

```text
add_node sensor as observation state raw weight 0.2
add_node planner as policy state idle
add_edge sensor -> planner relation informs weight 0.7
replace_node planner with controller
update_node controller state active
remove_edge sensor -> controller
remove_node scratch
find controller
traverse from sensor depth 2
balance graph
```

Graph expressions include `node_count`, `edge_count`, `graph_density`,
`traverse_hits`, `traverse_depth`, `causal_link_count`, and `message_count`.

## MapReduce Object Flows

MapReduce directives model bounded distributed transforms over the mutable
ontology. They record map, shuffle, partition, and reduce state and mirror the
flow into the graph with `map`, `shuffle`, `reduce`, and `emits` edges.

```text
mapreduce credit_flow over state_actions map emit_reward_pairs reduce merge_credit by object partitions 4
map reward_map over state_actions using emit_reward_pairs
shuffle reward_map by object partitions 3
reduce reward_map into credit_model using merge_credit
```

MapReduce expressions include `mapreduce_count`, `mapreduce_job_count`,
`map_count`, `shuffle_count`, `reduce_count`, and `partition_count`.

## SRM And Metrology State

SRM directives model NIST-style Standard Reference Materials, Reference
Materials, certificates, reports of investigation, safety data sheets,
traceability, calibration measurements, and uncertainty budgets. The construct
does not fetch product data; it records the agent's bounded metrology state and
mirrors it into the ontology graph.

```text
srm 2373 matrix genomic_dna property HER2 certificate current sds available traceable uncertainty 0.03
certificate 2373 current
report 2373
sds 2373 available
traceability 2373 to NIST
measurement her2_ratio on 2373 property HER2 value 2.1 uncertainty 0.03 unit ratio method ddPCR
calibration sequencer using 2373 uncertainty 0.02 method control_chart
uncertainty_budget her2_ratio = 0.03
quality_system NIST
```

Metrology expressions include `srm_count`, `reference_material_count`,
`certificate_count`, `current_certificate_count`, `sds_count`,
`traceability_count`, `measurement_count`, `calibration_count`,
`uncertainty_budget_count`, `mean_uncertainty`, and `max_uncertainty`.

## SRM Catalog And Ordering State

Operational SRM directives model catalog/store facts and ordering constraints
without performing network or payment operations.

```text
annual_product_list srm_product_list current
catalog online_catalog store shop.nist.gov current
product_search 2373 store shop.nist.gov found current
availability 2373 available orderable price 451.50 store shop.nist.gov
licensed_distributor standards_partner authorized
order_policy no_paper_checks
registration online required
survey customer required
shipping 2373 to Canada allowed
shipping 2373 to Russia blocked
archived_certificate 2373
```

Catalog/order expressions include `available_count`, `orderable_count`,
`product_search_count`, `catalog_count`, `annual_catalog_count`,
`licensed_distributor_count`, `order_policy_count`, `paper_checks_blocked`,
`registration_count`, `survey_count`, `shipping_block_count`,
`archived_certificate_count`, and `price_total`.

## Reward Dynamics And Adaptation

These constructs implement the 2024 OORL map: reward primitives are reified as
objects, reward learning is represented as goal alignment, and schema evolution
is exposed as policy optimization over a mutable object graph.

```text
reward_object completion valence 0.8 intensity 0.5 target state_action
causal_link state_action -> reward_sink weight 0.6
message state_action -> reward_sink weight 0.4
explore objects rate 0.35
credit state_action = 0.6
prune_edges below 0.2
attractor stable basin 0.25
prompt_game rewrite_game
```

Reward/adaptation expressions include `reward`, `valence`, `intensity`,
`exploration_rate`, `credit`, `pruning_threshold`, `basin_temperature`,
`reward_object_count`, `causal_link_count`, and `message_count`.

## Effects, Signals, And Refinement

```text
effect tool = 0.4
effect world = 0.5
effect meta = 0.1
reward = 0.3
curiosity = 0.4
empowerment = 0.2
confidence = 0.6
uncertainty = 0.7
learning_rate = 0.25
valence = 0.8
intensity = 0.5
exploration_rate = 0.35
credit = 0.6
pruning_threshold = 0.2
basin_temperature = 0.25

refine max_iterations += 2 when belief_count >= 1 and effect.world >= 0.5
```

Refinement rules are one-shot and run at clean model-done boundaries before
continue/break conditions are evaluated.

## Bounded Schema Rewrites

Schema rewrites are one-shot rules that apply any single DSL statement when a
condition becomes true. They run at clean model-done boundaries after numeric
refinements and before continue/break conditions, so a rewrite can change the
next prompt, add graph structure, or change loop limits before the flow decision.

```text
schema_rewrite add_edge state_action -> policy relation optimized weight 0.9 when credit >= 0.8
schema_rewrite max_iterations = 3 when reward >= 0.4
schema_rewrite prompt = "schema adapted" when reward_object_count == 1
continue when rewrites_applied >= 3 and edge_count >= 2
```

Aliases are accepted for agent-generated programs: `schema rewrite`,
`adapt_schema`, `adapt schema`, `rewrite_when`, and `rewrite`. Rewrite
expressions can inspect `rewrite_count`, `schema_rewrite_count`,
`rewrites_applied`, and `schema_rewrites_applied`.

## Example

```text
max_iterations = 3;
define(sensor, state_object);
add_node planner as policy state idle;
dyad sensor -> planner relation informs;
belief uncertainty = 0.7;
effect world = 0.5;
reward_object completion valence 0.8 intensity 0.5 target planner;
causal_link sensor -> planner weight 0.7;
explore objects rate 0.35;
traverse from sensor depth 2;
refine max_iterations += 2 when belief_count >= 1 and causal_link_count >= 1;
schema_rewrite add_edge planner -> reward_sink relation optimized weight 0.9 when credit >= 0.5;
mapreduce credit_flow over planner map emit_reward_pairs reduce merge_credit by objective partitions 4;
srm 2373 matrix genomic_dna property HER2 certificate current sds available traceable uncertainty 0.03;
measurement her2_ratio on 2373 value 2.1 uncertainty 0.03 unit ratio method ddPCR;
continue when iteration < max_iterations and reward_object_count >= 1;
break when turn >= 80
```
