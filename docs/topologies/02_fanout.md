# Category 2: Fan-Out / Fan-In (T09–T18)

## T09 — `starburst`
**Opus coordinates parallel Sonnet workers**
```
        ┌──▶ S(worker₁) ──┐
        ├──▶ S(worker₂) ──┤
O(coord)┤                  ├──▶ O(merge)
        ├──▶ S(worker₃) ──┤
        └──▶ S(worker₄) ──┘
```
- Opus decomposes, 4 Sonnets execute in parallel, Opus synthesizes
- Use: multi-file refactor, parallel feature implementation
- Est. latency: 3x | Agents: 6

## T10 — `scatter_gather`
**Sonnet dispatches Haiku fleet**
```
        ┌──▶ H(w₁) ──┐
        ├──▶ H(w₂) ──┤
S(split)┤──▶ H(w₃) ──├──▶ S(collect)
        ├──▶ H(w₄) ──┤
        ├──▶ H(w₅) ──┤
        └──▶ H(w₆) ──┘
```
- Cheap parallel execution, mid-tier collection
- Use: bulk file processing, parallel search, mass validation
- Est. latency: 3x | Agents: 8

## T11 — `mapreduce`
**Classic map-reduce with Haiku mappers**
```
        ┌──▶ H(map₁) ──┐
        ├──▶ H(map₂) ──┤
S(emit) ┤──▶ H(map₃) ──├──▶ S(reduce)
        ├──▶ H(map₄) ──┤
        ├──▶ H(map₅) ──┤
        ├──▶ H(map₆) ──┤
        ├──▶ H(map₇) ──┤
        └──▶ H(map₈) ──┘
```
- 8 Haiku mappers for maximum throughput, Sonnet reduces
- Use: large dataset analysis, codebase-wide search-and-transform
- Est. latency: 3x | Agents: 10

## T12 — `trident`
**Opus splits into 3 specialist Sonnets**
```
        ┌──▶ S(prong₁) ──┐
O(split)┤──▶ S(prong₂) ──├──▶ O(merge)
        └──▶ S(prong₃) ──┘
```
- Clean 3-way decomposition with heavy endpoints
- Use: frontend + backend + tests simultaneously, 3-perspective analysis
- Est. latency: 3x | Agents: 5

## T13 — `constellation`
**Opus bookends, Sonnet star cluster**
```
          ┌──▶ S(star₁) ──┐
          ├──▶ S(star₂) ──┤
O(design) ┤──▶ S(star₃) ──├──▶ O(synthesize)
          ├──▶ S(star₄) ──┤
          └──▶ S(star₅) ──┘
```
- 5 parallel Sonnet specialists — more parallelism than trident
- Use: comprehensive code review, multi-domain analysis
- Est. latency: 3x | Agents: 7

## T14 — `hydra`
**Two-level Haiku fan with Sonnet head**
```
        ┌──▶ H(head₁) ──┬──▶ H(sub₁)──┐
        │                └──▶ H(sub₂)──┤
S(neck) ┤                              ├──▶ S(merge)
        └──▶ H(head₂) ──┬──▶ H(sub₃)──┤
                         └──▶ H(sub₄)──┘
```
- Sonnet splits into 2 Haiku sub-coordinators, each spawns 2 workers
- Use: recursive directory processing, hierarchical data extraction
- Est. latency: 4x | Agents: 7

## T15 — `dandelion`
**Classify → route → judge**
```
                ┌──▶ S(spec₁)──┐
H(classify) ──▶ ├──▶ S(spec₂)──├──▶ O(judge)
  (routes 1)    ├──▶ S(spec₃)──┤
                └──▶ S(spec₄)──┘
```
- Haiku classifies, routes to ONE specialist Sonnet, Opus validates
- Only one Sonnet activates per run — conditional routing
- Use: domain-specific Q&A, support ticket routing
- Est. latency: 3x | Agents: 3 active (6 defined)

## T16 — `nova`
**Opus → Haiku burst → Sonnet merge → Opus review**
```
          ┌──▶ H₁ ──┐
          ├──▶ H₂ ──┤
          ├──▶ H₃ ──┤
O(plan) ──┤──▶ H₄ ──├──▶ S(merge) ──▶ O(review)
          ├──▶ H₅ ──┤
          ├──▶ H₆ ──┤
          ├──▶ H₇ ──┤
          └──▶ H₈ ──┘
```
- Maximum cheap parallelism with expensive bookends and mid-tier merge
- Use: mass code generation, parallel test writing, bulk transforms
- Est. latency: 4x | Agents: 11

## T17 — `fireworks`
**Two-stage fan with progressive collection**
```
        ┌──▶ H₁ ──┐           ┌──▶ S(coll₁)──┐
        ├──▶ H₂ ──┤──▶ stage ─┤              │
S(fuse) ┤──▶ H₃ ──┤    merge  ├──▶ S(coll₂)──├──▶ O(finale)
        ├──▶ H₄ ──┤           │              │
        ├──▶ H₅ ──┤           └──▶ S(coll₃)──┘
        └──▶ H₆ ──┘
```
- 6 Haiku burst → 3 Sonnet collectors (each takes 2) → Opus finale
- Use: multi-source research, parallel web scraping + analysis
- Est. latency: 4x | Agents: 11

## T18 — `prism`
**Spectral decomposition and recombination**
```
          ┌──▶ H(red)    ──┐
          ├──▶ H(orange) ──┤
          ├──▶ H(yellow) ──┤
S(split) ─┤──▶ H(green)  ──├──▶ S(recombine)
          ├──▶ H(blue)   ──┤
          ├──▶ H(indigo) ──┤
          └──▶ H(violet) ──┘
```
- Sonnet decomposes into 7 orthogonal aspects, Haiku handles each, Sonnet recombines
- Use: multi-aspect analysis (perf, security, style, docs, tests, types, deps)
- Est. latency: 3x | Agents: 9
