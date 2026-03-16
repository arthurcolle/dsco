# Category 3: Hierarchical Trees (T19–T26)

## T19 — `military`
**General → Captains → Soldiers**
```
              O(general)
             /          \
        S(capt₁)      S(capt₂)
       / |    \       / |    \
    H(s₁) H(s₂) H(s₃) H(s₄) H(s₅) H(s₆)
```
- Classic command hierarchy: Opus strategizes, Sonnets coordinate sectors, Haiku executes
- Use: large project decomposition, multi-module builds
- Est. latency: 3x | Agents: 9

## T20 — `corporate`
**CEO → VPs → Workers**
```
                  O(ceo)
               /    |    \
          S(vp₁)  S(vp₂)  S(vp₃)
          /|\      /|\      /|\
       H×3      H×3      H×3
```
- 3 Sonnet division heads each managing 3 Haiku workers
- Use: cross-team feature rollout, org-wide migrations
- Est. latency: 3x | Agents: 13

## T21 — `binary_tree`
**Balanced binary delegation**
```
              O(root)
            /        \
        S(L)          S(R)
       /    \        /    \
    H(LL)  H(LR)  H(RL)  H(RR)
```
- Perfect binary tree: divide-and-conquer
- Use: binary search over solution space, recursive problem splitting
- Est. latency: 3x | Agents: 7

## T22 — `asymmetric`
**Deep branch + wide branch**
```
         O(root)
        /       \
    S(deep)    H(w₁)
      |        H(w₂)
    S(deeper)  H(w₃)
      |
    H(leaf)
```
- Left: deep reasoning chain (S→S→H). Right: parallel breadth (3×H)
- Use: tasks needing both depth (algorithm design) and breadth (testing)
- Est. latency: 4x (deep) / 2x (wide) | Agents: 7

## T23 — `fractal`
**Self-similar recursive structure**
```
                O(root)
              /        \
          S(L)          S(R)
         /|\           /|\
      H  H  H      H  H  H
```
- Each tier is a smaller copy of the whole: 1→2→6
- Use: recursive data structures, tree-shaped problem domains
- Est. latency: 3x | Agents: 9

## T24 — `canopy`
**Deep spine with leaf clusters at each level**
```
    O(root) ──── H(leaf₁) H(leaf₂)
       |
    S(mid) ──── H(leaf₃) H(leaf₄)
       |
    S(low) ──── H(leaf₅) H(leaf₆)
       |
    H(ground)
```
- Vertical Opus→Sonnet→Sonnet spine, each node also fans out to 2 Haiku helpers
- Use: layered analysis with per-layer parallel data gathering
- Est. latency: 4x | Agents: 10

## T25 — `pyramid`
**Four-level widening pyramid**
```
          O(1)
         /   \  \
      S  S   S
     /|\ /|\ /|\
    H×3 H×3 H×3
```
- 1 Opus → 3 Sonnet → 9 Haiku (geometric expansion)
- Use: massive parallel execution with structured oversight
- Est. latency: 3x | Agents: 13

## T26 — `inverted_pyramid`
**Bottom-up aggregation**
```
    H H H H H H H H   (8 classifiers)
      \|/ \|/  \|/
      S    S    S  S   (4 aggregators)
        \  |  /
         S   S         (2 analysts)
          \ /
           O           (1 decider)
```
- Wide base of cheap classifiers narrows to expensive decider
- Use: survey analysis, multi-source intelligence fusion, voting systems
- Est. latency: 4x | Agents: 15
