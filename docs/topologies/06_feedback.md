# Category 6: Feedback / Iterative (T41–T48)

## T41 — `critic_loop`
**Generate → critique → refine (up to 3 rounds)**
```
    S(generate) ──▶ O(critique) ──▶ S(refine) ──┐
         ▲                                        |
         └────────── (if not approved) ───────────┘
```
- Sonnet generates, Opus critiques, Sonnet refines. Loop until Opus approves (max 3)
- Use: high-quality code generation, essay writing, API design
- Est. latency: 6-9x | Agents: 2 (3 iterations)

## T42 — `polish`
**Draft → review → fix → review cycle**
```
    H(draft) ──▶ S(review₁) ──▶ H(fix) ──▶ S(review₂)
                     ▲                           |
                     └── (cycle 2 if needed) ────┘
```
- Cheap draft + fix, expensive reviews. 2 cycles max
- Use: documentation, formatting tasks, template generation
- Est. latency: 4-8x | Agents: 2 (2 cycles)

## T43 — `adversarial`
**Red team vs Blue team with Opus judge**
```
    S(red:generate) ──▶ S(blue:attack) ──▶ S(red:defend) ──▶ O(judge)
```
- Red generates, Blue finds flaws, Red addresses them, Opus evaluates final quality
- Use: security review, contract review, robustness testing
- Est. latency: 4x | Agents: 3

## T44 — `evolution`
**Genetic algorithm pattern**
```
    Round 1:                      Round 2:
    H(gen₁) ──┐                  H(mutate₁) ──┐
    H(gen₂) ──├──▶ S(select) ──▶ H(mutate₂) ──├──▶ S(select₂) ──▶ O(final)
    H(gen₃) ──┤    (keep top 2)  H(mutate₃) ──┤
    H(gen₄) ──┘                               ──┘
```
- 4 Haiku generate random candidates, Sonnet selects fittest, mutate, select again, Opus picks winner
- Use: prompt engineering, config optimization, creative brainstorming
- Est. latency: 5x | Agents: 11

## T45 — `debate`
**Structured debate with moderator**
```
    S(pro) ──▶ S(con) ──▶ O(moderate) ──▶ winner argues again ──▶ O(decide)
```
- Pro argues for, Con argues against, Opus moderates. Winner gets second argument. Opus decides
- Use: controversial decisions, tradeoff analysis, buy-vs-build
- Est. latency: 5x | Agents: 3

## T46 — `annealing`
**Plan → implement → test → adjust → retest**
```
    O(plan) ──▶ S(implement) ──▶ H(test) ──▶ S(adjust) ──▶ H(retest)
                                    ▲                          |
                                    └── (if tests fail) ──────┘
```
- Start hot (Opus plans big), cool down through implementation and testing
- Feedback loop between test and adjust stages
- Use: TDD workflows, iterative bug fixing, optimization loops
- Est. latency: 5-8x | Agents: 4

## T47 — `ratchet`
**Incremental checkpoint-based progress**
```
    H(step₁) ──▶ S(checkpoint₁) ──▶ H(step₂) ──▶ S(checkpoint₂) ──▶ O(final)
```
- Haiku makes progress, Sonnet validates and locks in gains. Never go backward
- Use: long migration tasks, multi-step data transforms, incremental refactors
- Est. latency: 5x | Agents: 5

## T48 — `mirror`
**Generate → simplify → compare divergence**
```
    S(generate) ──▶ H(simplify) ──▶ S(compare) ──▶ O(decide)
         |                              ▲
         └─────── (original) ───────────┘
```
- Sonnet generates complex output, Haiku simplifies it, another Sonnet compares
  original vs simplified — if too divergent, the simplification lost meaning
- Opus decides which version ships
- Use: complexity reduction, API simplification, doc rewriting
- Est. latency: 4x | Agents: 4
