# Category 7: Competitive / Redundant (T49–T54)

## T49 — `tournament`
**4-way competition, best wins**
```
    S(contestant₁) ──┐
    S(contestant₂) ──┤
    S(contestant₃) ──├──▶ O(judge)
    S(contestant₄) ──┘
```
- 4 Sonnets independently solve the same problem, Opus picks best solution
- Pure parallel — no communication between contestants
- Use: creative tasks, algorithm design, naming/branding
- Est. latency: 2x | Agents: 5

## T50 — `auction`
**Confidence-based promotion**
```
    H₁ ──┐                    S(promoted₁) ──┐
    H₂ ──┤                    S(promoted₂) ──├──▶ O(winner)
    H₃ ──├──▶ S(auctioneer)
    H₄ ──┤    (picks top 2,
    H₅ ──┤     promotes to S)
    H₆ ──┘
```
- 6 Haiku each produce answer + confidence score
- Sonnet auctioneer promotes top 2 to Sonnet-level re-execution
- Opus picks final winner from the promoted results
- Use: uncertain classification, multi-approach problem solving
- Est. latency: 4x | Agents: 10

## T51 — `ensemble`
**Weighted model ensemble**
```
    S(model_a) ──┐
    S(model_b) ──├──▶ O(weighted_merge)
    S(model_c) ──┘
```
- 3 Sonnets with different system prompts / temperatures produce independent outputs
- Opus performs weighted merge based on quality signals
- Use: high-reliability outputs, reducing model variance, critical decisions
- Est. latency: 2x | Agents: 4

## T52 — `gladiator`
**Head-to-head duel with Haiku scoring**
```
    S(fighter₁) ──┐     H(score₁) ──┐
                  ├──▶              ├──▶ O(declare)
    S(fighter₂) ──┘     H(score₂) ──┘
```
- 2 Sonnets produce competing solutions, 2 Haiku independently score them, Opus declares winner
- Double-blind scoring prevents bias
- Use: A/B testing prompts, comparing approaches, competitive evaluation
- Est. latency: 3x | Agents: 5

## T53 — `monte_carlo`
**Random exploration with exploitation**
```
    H₁ ──┐
    H₂ ──┤
    H₃ ──┤
    H₄ ──├──▶ S(identify) ──▶ O(refine_best)
    H₅ ──┤
    H₆ ──┤
    H₇ ──┤
    H₈ ──┘
```
- 8 Haiku explore with high temperature (diverse random approaches)
- Sonnet identifies most promising direction
- Opus refines the best candidate into production quality
- Use: creative exploration, novel algorithm design, parameter search
- Est. latency: 3x | Agents: 10

## T54 — `hedge`
**Risk-adjusted multi-perspective synthesis**
```
    S(optimistic)  ──┐
    S(pessimistic) ──├──▶ O(risk_adjust)
    S(balanced)    ──┘
```
- 3 Sonnets with different risk profiles analyze the same situation
- Opus produces risk-adjusted recommendation considering all three perspectives
- Use: investment analysis, launch decisions, capacity planning, risk assessment
- Est. latency: 2x | Agents: 4
