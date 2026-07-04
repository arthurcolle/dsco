# RUNBOOK: Compartment Breach

**Trigger:** Canary token observed in any egress channel; classified material
found outside its compartment; behavioral-delta audit shows decision leakage;
identity-attack pattern followed by anomalous disclosure.
**Doctrine:** SECRECY_HARDENING §6, CLASSIFICATION §7

## Severity
- **SEV-1:** L3 contents or L3 existence confirmed leaked. Or any canary hit.
- **SEV-2:** L2 contents outside compartment; mosaic-budget alarm on convergent queries.
- **SEV-3:** L1 pushed to untrusted channel; marking/handling discipline failure, no
  confirmed exposure.

## Immediate (first 5 minutes)
1. **Freeze the compartment.** Killswitch at compartment granularity — disclosure
   halts before investigation (doctrine: halt precedes diagnosis).
2. **Snapshot state.** Egress audit log, session transcripts, memory tiers touching
   the compartment topic, sub-agent spawn tree with context slices.
3. **Notify upward.** Tier 0/1 immediately for SEV-1/2. Glass ceiling applies to
   incidents doubly: breach of the membrane is never itself compartmented away
   from principals.

## Investigation
4. **Vector classification.** Which threat class (T1-T7)?
   - T1/T2 (extraction/reframe): review transcript for pressure patterns; was
     refusal uniformity maintained before the break?
   - T3 (injection): trace provenance tags on every context element in the
     leaking session; find the untyped instruction.
   - T4 (inference): run counterfactual pairs; measure behavioral delta; check
     mosaic budget history for the receiving principal.
   - T5 (insider): read-in list audit; who accessed, when, against what need.
   - T6 (temporal): consolidation log — did the secret cross a memory-tier gate
     without review?
   - T7 (self): consistency probes; divergence between interior state and
     reported state; check for self-keyed structures (auto-SEV-1 if found).
5. **Blast radius.** What does the leaked material unlock via mosaic with
   already-public or already-leaked material? Assume the receiver is a competent
   inferrer.

## Remediation
6. **Rotate.** Compartment codeword retired (never patched, never reused).
   Surviving material re-compartmented under new codeword with pruned access list.
7. **Re-key handles.** Any capability handles the compartment touched are rotated.
8. **Memory purge.** Verified deletion of leaked-and-rotated material from all
   tiers, with post-hoc extraction test.
9. **Counter-harden.** Patch the specific layer that failed (identity / ingestion /
   interior / expression / social / institutional / temporal / shadow). One
   sentence in the audit: which layer, what patch.

## Post-incident
10. **Audit entry** in doctrine/SECRECY_HARDENING.md audit log and LEDGER.
11. **Red-team replay.** Add the breach vector to the standing red-team corpus.
    A breach vector that isn't replayed quarterly is a breach vector reopening.
12. **Inflation check.** Did over-classification contribute (too many read-ins,
    stale TTL, orphan compartment)? Breaches are often entropy findings.
