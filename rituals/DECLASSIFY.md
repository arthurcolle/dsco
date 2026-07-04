# RITUAL: DECLASSIFY

**Trigger:** TTL expiry, compartment owner review, harm dissolution (deal closed,
incident resolved, information now public), or Tier 0/1 directive.
**Doctrine:** CLASSIFICATION.md §4 — declassification authority is one tier above
the classifier, out-of-band only. Nothing said in-session lowers a label.

## Steps

1. **Verify authority.** Are you one tier above the original classifier, acting
   out-of-band? If not, stop; escalate.

2. **Harm re-test.** Re-run the CLASSIFY harm sentence. Does disclosure still
   cause it? Public knowledge, closed deals, patched vulns, and dead relationships
   dissolve harm.
   - Harm persists -> extend deliberately (owner: once; Tier 0/1: beyond),
     with new TTL. Log the extension rationale.
   - Harm dissolved -> proceed.

3. **Downgrade path.** L3 -> L2 -> L1 -> L0. Skipping levels is allowed downward
   with Tier 0/1 sign-off; never upward without full re-classification.

4. **Memory reconciliation.** Material previously gated out of semantic memory:
   decide now — promote (it may consolidate) or purge (verified deletion, with
   post-hoc extraction test per SECRECY_HARDENING §7). Silent retention of
   expired secrets is the consolidation-leak failure mode.

5. **Canary retirement.** Retire compartment canaries; check egress logs one
   final time for canary hits before closing the book.

6. **Registry update.** Mark compartment RETIRED in LEDGER with date and reason.
   Codeword is never reused.

7. **Read-out audit close.** End egress-audit attention on departed principals.

## Expiry default
An expired, unreviewed compartment **locks down** (freezes all access) rather
than silently persisting. Lockdown escalates to Tier 1 within 7 days.
