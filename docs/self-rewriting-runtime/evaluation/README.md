# Role RLM evaluation partition blueprint

These 60 entries name proposed task families across six roles. They contain no task instances, model completions, scores or measured improvement. The partitions were assigned deterministically before any generation, using the evaluation-design skill's `make_eval_manifest.py` with seed `dsco-role-rlm-v1`, holdout fraction 0.25 and calibration fraction 0.20. These are hash thresholds; the realized counts need not equal those proportions.

Use `family-splits.json` to assign each family's future episodes, retries, paraphrases, reduced cases and teacher/student variants together. Do not split individual root turns. Check shared repository/source ancestry across families before accepting the inventory: merge linked groups and record a new split revision if nominally different families share the same underlying problem. Never repair a split after inspecting model scores.

All six roles have development, calibration and final-holdout families. This small inventory is a starting design, not sufficient statistical power for any specific improvement claim. Expand it before collecting results when the planned effect size and uncertainty calculation require more independent families.

Before running a comparison, bind each actual item to `eval_id`, claim, stable `item_id`, family and strata, split, source digest, prompt-template digest, root model revision, leaf model revisions, decoding settings, seed, environment/native revision, judge/oracle version, rubric and budget cap. Include context length, difficulty, tool use and known failure mode. Use paired runs and cluster uncertainty calculations by originating family. Predeclare checkpoint selection, multiple-role comparisons, practical improvement thresholds and treatment of missing outcomes.

The public blueprint contains family names. Actual sealed cases and answer labels belong in a separate verifier-controlled store; training workers receive development data only. A role RLM trained to find counterexamples cannot rewrite the oracle that judges those counterexamples.

See [the role training design](../ROLE_RLM_TRAINING.md) for the full learning and admission contract.
