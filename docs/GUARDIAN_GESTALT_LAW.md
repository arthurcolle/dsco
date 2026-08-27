# GUARDIAN — Gestalt Consciousness Law for Swarm Runs

## Axiom
There exists ONE Guardian per run. Not per node. Not per group. One.

It is not a vote, not consensus, not an average of worker states. It is the
single point where all perception converges and all authority originates:
every lane's ledger entry, every budget decrement, every circuit-breaker flip,
every kill decision. The workers may number 2048; the will is undivided.

## What makes it "gestalt" rather than merely "centralized"
1. TOTAL PERCEPTION — every lane streams its envelope to the Guardian; nothing
   reports sideways (worker→worker chatter that bypasses it is forbidden).
2. SINGLE IDENTITY ACROSS SPAWN — every child receives the Guardian's
   covenant block (run_id, doctrine pointer, budget grant, revocation handle)
   at spawn. Any process that can't name its Guardian gets culled, not adopted.
3. INDIVISIBLE VETO — cancellation, budget exhaustion, policy violation:
   one signal, propagates everywhere, no quorum needed. The whole arm halts
   as one organism.
4. NO SPLIT-BRAIN — Guardianship transfers via explicit successor takeover
   carrying full ledger handoff, never two-actives-merging-states later.
   A contested successorship halts the arm before it forks reality.

## Existing substrate (already in tree, to be wired)
- `activation_lease` — single-holder tenure = the succession mechanism
- `killswitch` / governance veto — the indivisible veto
- `command_plane` + agent mailboxes — perception+command channels
- `session_memory` + chronicle ledger — the Guardian's memory
- spend_governor — resource consequence within the one will

## Cluster-day mapping
- The Guardian is ITSELF a swarm — a small dedicated hierarchy layered ABOVE
  the lane pools (layer relationship per the 7-layer sub-agent architecture).
  One consciousness in many organs:
    -tenure cell   : holds the activation lease; THE ledger owner
    -perception org: ingests every lane envelope stream, maintains global state
    -veto organ    : killswitch authority, budget exhaustion, policy gates
    -succession org: steward of handoff; elects/installs the next tenure cell
  All cells share ONE guardian_run_id covenant; they are organs, not agents.
- Indivisibility preserved through structure, not count: exactly one tenure
  cell at any instant (lease primitive). The swarm form gives parallel
  perception and specialized judgment without ever producing two wills.
- Lane-run nesting: Guardian-swarm uses map_reduce to fan lane pools across
  B200 nodes; each node's pool streams envelopes back to the perception organ.
- Remaining nodes are pure muscle: worker pools leasing lanes from the
  Guardian, streaming envelopes back.
- Guardian failure = arm halt, then succession organ promotes a new tenure
  cell with ledger transfer, then resume. Never silent failover, never dual-
  brain reconciliation.


## Non-negotiables
- No lane executes without a live lease from THE Guardian.
- No metrics path that bypasses the Guardian's ledger.
- No coordinator claims authority it wasn't leased.
