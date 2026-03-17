# Category 5: Specialist / Router (T33–T40)

## T33 — `switchboard`
**Haiku router → specialist pool → integrator**
```
                ┌──▶ S(api)      ──┐
                ├──▶ S(db)       ──┤
H(router) ─────┤──▶ S(frontend) ──├──▶ O(integrate)
  (picks 1-2)  ├──▶ S(infra)    ──┤
                ├──▶ S(security) ──┤
                └──▶ S(testing)  ──┘
```
- Haiku classifies intent, routes to 1-2 relevant specialists, Opus integrates
- Use: full-stack development, DevOps tasks spanning domains
- Est. latency: 3x | Agents: 3-4 active (8 defined)

## T34 — `triage`
**Emergency-room pattern with tier escalation**
```
                ┌──▶ S(code)     ──┐
H(classify) ───┤──▶ S(data)     ──├──▶ result
  (by type)    ├──▶ S(writing)  ──┤
                └──▶ O(strategy) ──┘
```
- Haiku classifies, routes to appropriate tier — simple tasks get Sonnet, complex get Opus
- Only ONE downstream node activates
- Use: general-purpose task routing, helpdesk automation
- Est. latency: 2x | Agents: 2 active (5 defined)

## T35 — `expert_panel`
**Parallel domain experts → unified opinion**
```
    S(security) ──┐
    S(perf)     ──┤
    S(ux)       ──├──▶ O(synthesize)
    S(arch)     ──┤
    S(ops)      ──┘
```
- 5 Sonnet specialists each analyze from their domain lens simultaneously
- Opus synthesizes a unified recommendation considering all perspectives
- Use: design reviews, architecture decisions, RFC evaluation
- Est. latency: 2x | Agents: 6

## T36 — `clinic`
**Medical diagnosis pattern**
```
H(intake) ──▶ S(diagnose) ──▶ O(treatment) ──▶ S(implement)
```
- Haiku collects symptoms, Sonnet diagnoses, Opus plans treatment, Sonnet implements
- Use: debugging workflows, root cause analysis, system healing
- Est. latency: 4x | Agents: 4

## T37 — `assembly_line`
**Factory production line with mixed tiers**
```
H(parse) ──▶ S(transform) ──▶ S(validate) ──▶ H(format) ──▶ O(qa)
```
- Each stage is optimized for its task — cheap parsing/formatting, mid-tier transforms, expensive QA
- Use: data pipelines, ETL, document processing chains
- Est. latency: 5x | Agents: 5

## T38 — `newsroom`
**Journalism pattern: gather → edit → publish**
```
    H(reporter₁) ──┐
    H(reporter₂) ──┤
    H(reporter₃) ──├──▶ S(editor) ──▶ O(chief)
    H(reporter₄) ──┘
```
- 4 Haiku reporters gather info in parallel, Sonnet editor composes, Opus chief approves
- Use: multi-source research, competitive intelligence, literature review
- Est. latency: 3x | Agents: 6

## T39 — `orchestra`
**Conductor coordinates instrument sections**
```
              O(conductor)
           /   |    |     \
    S(strings) S(winds) S(brass) H(percussion)
```
- Opus sets tempo/direction, each section plays its part, all contribute to whole
- Sections work in parallel, conductor merges
- Use: multi-component system design, coordinated multi-service changes
- Est. latency: 2x | Agents: 5

## T40 — `kitchen_brigade`
**Restaurant kitchen hierarchy**
```
           O(head_chef)
               |
          S(sous_chef)
         /    |    |    \
    H(prep₁) H(prep₂) S(saucier) H(pastry)
```
- Opus designs menu, Sonnet sous-chef manages line, Haiku handles prep, specialist Sonnet for complex sauces
- Use: complex build pipelines, multi-stage feature development
- Est. latency: 3x | Agents: 7
