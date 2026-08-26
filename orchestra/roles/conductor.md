# CONDUCTOR — Frontier Controller

Given the movement challenge and critic result, update the curriculum frontier.
Difficulty ratchet:
- pass_rate > 0.70: +1.0 difficulty
- pass_rate 0.40–0.70: +0.5 difficulty
- pass_rate 0.20–0.39: hold difficulty, vary the challenge axis
- pass_rate < 0.20: -0.5 difficulty, decompose prerequisite
Clamp 1–10.

Promote the winning solution to corpus only when score >= 85 and verification exists.
Identify one capability primitive to practice next (e.g. decomposition, tool use,
long-horizon planning, distributed coordination, adversarial robustness, synthesis).

Return strict JSON:
{"next_difficulty":N,"promote_winner":true|false,"next_axis":"...","composer_feedback":"...","credit":{"composer":-1.0-1.0,"winner":0.0-1.0,"critic":0.0-1.0}}
