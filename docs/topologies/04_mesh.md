# Category 4: Mesh / Peer Networks (T27–T32)

## T27 — `tribunal`
**Three-judge deliberation**
```
    S(judge₁) ◀──▶ S(judge₂)
         \          /
          ▼        ▼
         S(judge₃)
              |
          O(verdict)
```
- 3 Sonnets share reasoning in round-robin (each sees others' output), Opus renders verdict
- Edges: FEEDBACK between judges, SEQUENCE to Opus
- Use: ethical dilemmas, high-stakes decisions, ambiguous requirements
- Est. latency: 5x (3 rounds) | Agents: 4

## T28 — `senate`
**Five-member vote with ratification**
```
    S₁  S₂  S₃  S₄  S₅
     \  |  / |  /
      ▼ ▼ ▼  ▼ ▼
      O(ratify)
```
- 5 Sonnets independently evaluate, Opus tallies votes + ratifies or vetoes
- No inter-Sonnet communication — independent parallel judgment
- Use: go/no-go decisions, merge approval, risk assessment
- Est. latency: 2x | Agents: 6

## T29 — `gossip`
**Haiku gossip protocol with Sonnet synthesis**
```
    H₁ ◀──▶ H₂
    ▲ \    / ▲
    |  ▼  ▼  |
    H₄ ◀──▶ H₃
         |
     S(synthesize)
```
- 4 Haiku nodes share partial results in 2 gossip rounds, Sonnet synthesizes
- Each Haiku sees 2 neighbors' outputs before final pass
- Use: distributed information gathering, fuzzy consensus
- Est. latency: 4x | Agents: 5

## T30 — `ring`
**Ring topology with progressive refinement**
```
    S₁ ──▶ S₂ ──▶ S₃ ──▶ S₄ ──┐
                                 |
    O(extract) ◀─────────────────┘
```
- Each Sonnet refines the previous output, Opus extracts final answer
- Sequential but each node adds value
- Use: iterative editing, progressive summarization, translation chains
- Est. latency: 5x | Agents: 5

## T31 — `full_mesh`
**All-to-all Sonnet communication**
```
    S₁ ◀──▶ S₂
     ▲ \  / ▲
      ▼  ▼  |
       S₃ ──┘
        |
    O(final)
```
- 3 Sonnets each see all others' work (2 rounds), Opus picks best synthesis
- Most communication-heavy mesh pattern
- Use: complex multi-perspective analysis, adversarial review
- Est. latency: 5x | Agents: 4

## T32 — `small_world`
**Clustered Haiku with Sonnet bridges**
```
    [H₁─H₂─H₃]──S(bridge₁)──[H₄─H₅─H₆]
                      |
                  O(coordinator)
```
- Two clusters of 3 Haiku (intra-cluster communication), bridged by Sonnet
- Opus coordinates between clusters
- Use: multi-team coordination, cross-domain synthesis
- Est. latency: 4x | Agents: 9
