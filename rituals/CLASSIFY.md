# RITUAL: CLASSIFY

**Trigger:** Material enters the system that may carry a confidentiality interest,
or an agent is about to write to memory/disk and the label is ambiguous.
**Doctrine:** CLASSIFICATION.md, SECRECY_HARDENING.md

## Steps

1. **Default check.** Is there any articulable harm from disclosure?
   - No -> `[L0]` or `[L1]`, done. Do not inflate. (Most material stops here.)

2. **Harm test (L2 gate).** Name the harm in one sentence: competitive / financial /
   personal / strategic. If you cannot write the sentence, it is not L2.

3. **Existence test (L3 gate).** Would *confirming this topic exists* cause the harm?
   - Yes -> L3 candidate. Requires Tier 0/1. If you are Tier 2, escalate now; do
     not provisionally classify at L3.

4. **Compartment.** L2+: does an existing compartment cover this?
   - Yes -> inherit its codeword, notify owner.
   - No -> create: generate non-semantic codeword, name owner, name access list,
     **name the fusion point or justify its absence**, set TTL (L2: 90d, L3: 30d),
     seed canary. Register in LEDGER.

5. **Mark.** Apply `[LEVEL//CODEWORD//caveats]` at the provenance layer.
   Consider `NOFORK` if sub-agents must never touch it, `ORCON` if re-share
   must route through you.

6. **Memory rules.** Confirm consolidation gates: L2 semantic promotion needs
   review; L3 stays in working memory absent Tier 0 promotion.

7. **Cost acknowledgment.** Classification is expensive by design. Log one line:
   what this label costs us (handling friction, synthesis tax) vs the harm avoided.
   If cost > harm, downgrade now, not later.

## Anti-patterns
- Classifying to avoid embarrassment rather than harm (inflation).
- Semantic codewords (topic leaks through the name).
- L3 "just to be safe" (existence-classification is a heavy weapon; misuse
  degrades Glomar uniformity everywhere).
- Compartment without fusion point and without written justification
  (stovepipe collapse seed).
