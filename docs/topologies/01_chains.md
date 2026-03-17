# Category 1: Linear Chains (T01–T08)

## T01 — `sentinel`
**Escalation chain: triage → analyze → decide**
```
H(triage) ──▶ S(analyze) ──▶ O(decide)
```
- Haiku classifies urgency, Sonnet investigates, Opus makes strategic call
- Use: incident response, support escalation, bug triage
- Est. latency: 3x | Agents: 3

## T02 — `refinery`
**Multi-stage distillation**
```
H(filter₁) ──▶ H(filter₂) ──▶ S(synthesize)
```
- Two cheap Haiku passes strip noise, Sonnet produces clean output
- Use: log analysis, data cleaning, document summarization
- Est. latency: 3x | Agents: 3

## T03 — `deepdive`
**Scout → reason → implement**
```
S(scout) ──▶ O(reason) ──▶ S(implement)
```
- Sonnet gathers context, Opus forms strategy, Sonnet executes plan
- Use: complex refactors, architecture decisions, research synthesis
- Est. latency: 3x | Agents: 3

## T04 — `cascade`
**Top-down delegation**
```
O(plan) ──▶ S(implement) ──▶ H(format)
```
- Opus plans, Sonnet builds, Haiku polishes output
- Use: document generation, feature specs → code → tests
- Est. latency: 3x | Agents: 3

## T05 — `echo`
**Triple verification pipeline**
```
S(draft) ──▶ S(review) ──▶ S(finalize)
```
- Three independent Sonnet passes — generate, review, fix
- Use: high-stakes code generation, contract drafting
- Est. latency: 3x | Agents: 3

## T06 — `distillery`
**Wide-to-narrow extraction**
```
H(pass₁) ──▶ H(pass₂) ──▶ H(pass₃) ──▶ S(distill)
```
- Three Haiku stages progressively compress, Sonnet extracts essence
- Use: large corpus summarization, multi-doc Q&A
- Est. latency: 4x | Agents: 4

## T07 — `telescope`
**Zoom in, zoom out**
```
H(scan) ──▶ S(focus) ──▶ O(deep) ──▶ S(contextualize) ──▶ H(format)
```
- Progressive depth increase then decrease — cheap bookends, expensive core
- Use: codebase exploration, security audit with summary
- Est. latency: 5x | Agents: 5

## T08 — `gauntlet`
**Challenge-response endurance chain**
```
H(parse) ──▶ S(challenge) ──▶ O(defend) ──▶ S(verify) ──▶ H(score) ──▶ H(report)
```
- Input parsed, challenged, defended, verified, scored, reported
- Use: claim verification, argument stress-testing, code hardening
- Est. latency: 6x | Agents: 6
