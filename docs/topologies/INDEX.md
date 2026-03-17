# dsco Topology Registry — 60 Custom Agent Topologies

**Models:** O = Opus 4.6 | S = Sonnet 4.6 | H = Haiku 4.5

## Category 1: Linear Chains
| ID | Name | Pattern | Agents | Latency |
|----|------|---------|--------|---------|
| T01 | `sentinel` | H→S→O | 3 | 3x |
| T02 | `refinery` | H→H→S | 3 | 3x |
| T03 | `deepdive` | S→O→S | 3 | 3x |
| T04 | `cascade` | O→S→H | 3 | 3x |
| T05 | `echo` | S→S→S | 3 | 3x |
| T06 | `distillery` | H→H→H→S | 4 | 4x |
| T07 | `telescope` | H→S→O→S→H | 5 | 5x |
| T08 | `gauntlet` | H→S→O→S→H→H | 6 | 6x |

## Category 2: Fan-Out / Fan-In
| ID | Name | Pattern | Agents | Latency |
|----|------|---------|--------|---------|
| T09 | `starburst` | O→4×S→O | 6 | 3x |
| T10 | `scatter_gather` | S→6×H→S | 8 | 3x |
| T11 | `mapreduce` | S→8×H→S | 10 | 3x |
| T12 | `trident` | O→3×S→O | 5 | 3x |
| T13 | `constellation` | O→5×S→O | 7 | 3x |
| T14 | `hydra` | S→2×H→2×H→S | 7 | 4x |
| T15 | `dandelion` | H→(1of4)S→O | 3-6 | 3x |
| T16 | `nova` | O→8×H→S→O | 11 | 4x |
| T17 | `fireworks` | S→6×H→3×S→O | 11 | 4x |
| T18 | `prism` | S→7×H→S | 9 | 3x |

## Category 3: Hierarchical Trees
| ID | Name | Pattern | Agents | Latency |
|----|------|---------|--------|---------|
| T19 | `military` | O→2×S→6×H | 9 | 3x |
| T20 | `corporate` | O→3×S→9×H | 13 | 3x |
| T21 | `binary_tree` | O→2×S→4×H | 7 | 3x |
| T22 | `asymmetric` | O→(S→S→H)+(3×H) | 7 | 4x |
| T23 | `fractal` | O→2×S→6×H | 9 | 3x |
| T24 | `canopy` | O(+2H)→S(+2H)→S(+2H)→H | 10 | 4x |
| T25 | `pyramid` | O→3×S→9×H | 13 | 3x |
| T26 | `inverted_pyramid` | 8×H→4×S→2×S→O | 15 | 4x |

## Category 4: Mesh / Peer Networks
| ID | Name | Pattern | Agents | Latency |
|----|------|---------|--------|---------|
| T27 | `tribunal` | 3×S(round-robin)→O | 4 | 5x |
| T28 | `senate` | 5×S(parallel vote)→O | 6 | 2x |
| T29 | `gossip` | 4×H(gossip)→S | 5 | 4x |
| T30 | `ring` | 4×S(ring)→O | 5 | 5x |
| T31 | `full_mesh` | 3×S(all-to-all)→O | 4 | 5x |
| T32 | `small_world` | 2×(3×H cluster)+S bridge→O | 9 | 4x |

## Category 5: Specialist / Router
| ID | Name | Pattern | Agents | Latency |
|----|------|---------|--------|---------|
| T33 | `switchboard` | H→(1-2of6)S→O | 3-8 | 3x |
| T34 | `triage` | H→{S,S,S,O} | 2-5 | 2x |
| T35 | `expert_panel` | 5×S(parallel)→O | 6 | 2x |
| T36 | `clinic` | H→S→O→S | 4 | 4x |
| T37 | `assembly_line` | H→S→S→H→O | 5 | 5x |
| T38 | `newsroom` | 4×H→S→O | 6 | 3x |
| T39 | `orchestra` | O→{3×S+H}→merge | 5 | 2x |
| T40 | `kitchen_brigade` | O→S→{2×H+S+H} | 7 | 3x |

## Category 6: Feedback / Iterative
| ID | Name | Pattern | Agents | Latency |
|----|------|---------|--------|---------|
| T41 | `critic_loop` | S⇄O (×3) | 2 | 6-9x |
| T42 | `polish` | H→S→H→S (×2) | 2 | 4-8x |
| T43 | `adversarial` | S(red)→S(blue)→S(red)→O | 3 | 4x |
| T44 | `evolution` | 4×H→S→3×H→S→O | 11 | 5x |
| T45 | `debate` | S(pro)→S(con)→O→loop→O | 3 | 5x |
| T46 | `annealing` | O→S→H⇄S→H | 4 | 5-8x |
| T47 | `ratchet` | H→S→H→S→O | 5 | 5x |
| T48 | `mirror` | S→H→S(compare)→O | 4 | 4x |

## Category 7: Competitive / Redundant
| ID | Name | Pattern | Agents | Latency |
|----|------|---------|--------|---------|
| T49 | `tournament` | 4×S(compete)→O | 5 | 2x |
| T50 | `auction` | 6×H→S(promote 2)→O | 10 | 4x |
| T51 | `ensemble` | 3×S(diverse)→O(merge) | 4 | 2x |
| T52 | `gladiator` | 2×S+2×H(score)→O | 5 | 3x |
| T53 | `monte_carlo` | 8×H→S→O | 10 | 3x |
| T54 | `hedge` | 3×S(risk profiles)→O | 4 | 2x |

## Category 8: Domain-Specific
| ID | Name | Pattern | Agents | Latency |
|----|------|---------|--------|---------|
| T55 | `code_review` | H→S→S→O | 4 | 4x |
| T56 | `research` | 4×H→2×S→O | 7 | 3x |
| T57 | `incident` | H→S→O→S→H | 5 | 5x |
| T58 | `data_pipeline` | H→H→S→S→O | 5 | 5x |
| T59 | `security_audit` | 3×H→S→O→S | 6 | 4x |
| T60 | `creative` | 6×H→S→O→S→H | 10 | 5x |

---

## Summary Statistics
- **Total topologies:** 60
- **Avg agents/topology:** ~6.2
- **Cheapest (H-heavy):** T02 refinery, T06 distillery, T11 mapreduce
- **Most capable (O-heavy):** T03 deepdive, T07 telescope, T41 critic_loop
- **Fastest (low latency):** T28 senate (2x), T34 triage (2x), T35 expert_panel (2x)
- **Most parallel:** T16 nova (8-way), T11 mapreduce (8-way), T53 monte_carlo (8-way)
- **Most iterative:** T41 critic_loop (up to 9x), T46 annealing (up to 8x)
