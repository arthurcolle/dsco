# PRAXIS-v1 promotion gate (must pass IN ORDER before any propagation)

- [ ] 1. SHADOW: run PRAXIS as the control loop in shadow alongside OVERMIND on >=20 real
        tasks. Emit a shadow-run log artifact (per-task EV, governor projections, ledger deltas).
- [ ] 2. VERIFY: no regression vs OVERMIND on the 80 doctrines + failure-mode suite; calibration
        (PIT/Brier) not worse; ledger net-positive. Emit verify-report.json.
- [ ] 3. SIGN: human-signed promotion token (the germline signed pipeline). Without it,
        OVERMIND stays canonical.
- [ ] 4. PROPAGATE: only then remap doctrine/, edit SOUL.md/IDENTITY.md/ANATOMY.md, and lay a
        *typed* migration gradient — with a verified rollback path committed to git first.

Rollback (always available): rm PRAXIS_ARCHITECTURE.md (untracked) / restore .orig; the field decays.
