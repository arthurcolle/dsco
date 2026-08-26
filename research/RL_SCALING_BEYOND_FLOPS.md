# A Compact Framework for RL Scaling in LLM Agents Beyond FLOPs

**Status:** independent derivation, literature-anchored
**Scope:** post-training RL (RLVR / agentic RL), not pre-training
**Date:** 2026-07-09

---

## 0. The problem with a FLOP-only law

The current best predictive result for RL post-training is a *saturating sigmoid in compute*, not a power law. Khatri, Madaan et al. (ScaleRL, "The Art of Scaling Reinforcement Learning Compute for LLMs," arXiv:2510.13786, 2025) fit

$$
R_C - R_0 = (A - R_0)\cdot \frac{1}{1 + (C_{\text{mid}}/C)^{B}}
$$

where $C$ is RL compute (GPU-hours ≈ FLOPs), $A$ is the **asymptotic pass rate**, $B$ the **compute-efficiency exponent**, $C_{\text{mid}}$ the midpoint. Their central empirical claim: most popular design choices (loss aggregation, normalization, curriculum, off-policy algorithm) move only $B$ and $C_{\text{mid}}$ — the *efficiency* — while a smaller set (loss type, precision, batch size) move the *ceiling* $A$. Nathan Lambert's read (interconnects.ai, "How to scale RL," 2025) sharpens the interpretation: unlike the pretraining loss power law, this curve is bounded (accuracy, not next-token loss), so it is most useful for **ablating design choices**, not for revealing a fundamental law of nature.

The deep limitation both sources flag: **the law holds "for fixed model and training data."** It says nothing about *what to scale besides compute*. The ceiling $A$ itself is a function of the data/environment, the task horizon, the reward channel, and the exploration process. A framework "beyond FLOPs" must make $A$, $B$, $C_{\text{mid}}$ *functions of non-compute resources* — and identify which resource each axis actually moves.

---

## 1. Independent derivation: the resource-vector generalization

### 1.1 The object we are scaling

An RL post-training run consumes a **resource vector**, not a scalar:

$$
\mathbf{r} = (C,\; N_{\text{env}},\; H,\; \phi,\; \nu,\; \rho,\; \sigma)
$$

- $C$ — compute (FLOPs / GPU-hours)
- $N_{\text{env}}$ — data/environment diversity (effective number of distinct tasks/levels)
- $H$ — task horizon (decision steps per episode before terminal reward)
- $\phi$ — feedback density / credit-assignment resolution (informative bits of reward per decision)
- $\nu$ — verification fidelity (probability the reward channel is *correct*)
- $\rho$ — reset/replay intensity (data reuse ratio + weight-reset schedule)
- $\sigma$ — exploration diversity (behavioral entropy / coverage of the solution space)

The claim is that the ScaleRL constants factorize:

$$
A = A(N_{\text{env}}, \nu, \sigma, H), \qquad
B = B(\phi, \rho, \sigma), \qquad
C_{\text{mid}} = C_{\text{mid}}(H, \phi, \rho).
$$

That is: **diversity, verification fidelity, and sustained exploration set the ceiling; feedback density, replay, and exploration set the slope; horizon and feedback shift the midpoint (where the compute has to go).** This is the falsifiable spine of the framework.

### 1.2 A single scalar: the effective sample

Define the **effective learning sample** $S_{\text{eff}}$ — the quantity of *non-redundant, correctly-credited, correctly-verified* gradient signal actually extracted, per unit of raw rollout $n$:

$$
S_{\text{eff}} = n \cdot \underbrace{d(N_{\text{env}})}_{\text{diversity yield}} \cdot \underbrace{\nu}_{\text{verify}} \cdot \underbrace{g(\phi, H)}_{\text{credit efficiency}} \cdot \underbrace{u(\rho)}_{\text{reuse factor}} \cdot \underbrace{c(\sigma)}_{\text{coverage}}
$$

Each multiplier is a leak. RL post-training is a chain of lossy channels between raw rollouts and useful gradient; the *minimum* multiplier is the binding constraint. This gives the framework its operational bite: **the axis that is smallest is the one to scale next**, regardless of how much FLOPs you add. Adding compute when $\nu \approx 0.7$ (30% reward is wrong) just amplifies reward hacking.

Below, each multiplier is derived and tied to literature.

---

## 2. The seven axes, derived and evaluated

### Axis 1 — Data / environment diversity $N_{\text{env}} \Rightarrow d(N_{\text{env}})$, sets $A$

**Derivation.** RL generalization is governed by the *number of distinct environments seen*, not the number of steps. Cobbe et al. (Procgen, ICML 2020) established the canonical result: with too few procedurally-generated levels, agents memorize; a generalization gap opens that closes only as level count grows — sample efficiency and generalization are *distinct* axes measured on the same suite. Unsupervised Environment Design (PAIRED; PLR; ACCEL; and successors like PACE, arXiv:2605.01358) formalizes that *which* environments you present — at the frontier of the agent's competence (regret-maximizing / learning-progress-maximizing) — matters more than raw count. ScaleRL's own multi-task extension (math + code) shows the asymptote $A$ rising with task variety while remaining predictable.

Model the yield as saturating in *effective* diversity (distinct levels weighted by how many are still learnable):

$$
d(N_{\text{env}}) = 1 - e^{-N_{\text{eff}}/N_0}, \qquad N_{\text{eff}} = \sum_i w_i,\; w_i = \text{learning progress on env } i.
$$

**Evaluation.** Strongly supported and arguably the dominant lever on $A$. Lambert explicitly names "no information on the impacts of different data" and "choosing the right base model" as ScaleRL's two biggest open holes — i.e. the community *cannot yet* fit $A(N_{\text{env}})$. **Verdict: real, first-order, under-measured.**

### Axis 2 — Horizon $H \Rightarrow$ shifts $C_{\text{mid}}$ up, caps $A$ when credit fails

**Derivation.** Longer horizons dilute a fixed terminal reward across more decisions, so the signal-per-decision falls as $\sim 1/H$ absent credit-assignment machinery. The horizon-length scaling analysis (LessWrong, "The effect of horizon length on scaling laws," 2023; over Procgen, Dota 2, toy MNIST) shows that the compute needed scales with an effective horizon term — you pay more FLOPs to reach the same performance as horizon grows, i.e. $C_{\text{mid}}$ shifts right. ScaleRL confirms generation length (32,768 tokens) as a *predictable* compute axis: longer traces remain on-curve but cost more to climb. Lambert separately flags "very long horizon" RL (scientific-discovery-scale) as qualitatively beyond current models.

$$
C_{\text{mid}}(H) \propto H^{\alpha},\quad \alpha \in (0,1] \text{ (empirically to fit)}; \qquad A_{\max}(H) \to A_0 \text{ only if } \phi \text{ scales with } H.
$$

**Evaluation.** Well-motivated; horizon is a *cost multiplier* (moves $C_{\text{mid}}$), and becomes a *ceiling killer* only when credit assignment fails to keep pace. This is why Axes 2 and 3 are entangled and must be ablated jointly. **Verdict: real, but its effect on $A$ is mediated by Axis 3 — not independent.**

### Axis 3 — Feedback / credit assignment $\phi \Rightarrow g(\phi,H)$, sets $B$, shifts $C_{\text{mid}}$

**Derivation.** With only a sparse terminal reward, the per-decision gradient variance grows with $H$; credit assignment is the machinery that concentrates the signal onto the responsible decisions. Recent LLM-agent work makes this explicit: Hindsight Credit Assignment for Long-Horizon LLM Agents (arXiv:2603.08754, 2026); intrinsic/belief-based credit assignment (alphaXiv, 2026); Microsoft's Agent Lightning (2025) attaches input/context/output/reward to *each agent call* so the credit-assignment module can score sub-steps; the Awesome-Credit-Assignment-in-LLM-RL corpus frames the core question as "which actions were responsible when only a sparse terminal reward exists." Model credit efficiency as recovering the dense-reward slope as feedback resolution $\phi$ rises:

$$
g(\phi, H) = \frac{\phi}{\phi + H}, \qquad B \propto g(\phi,H).
$$

At $\phi \gg H$ (per-step verifiable feedback) you recover near-dense-reward efficiency; at $\phi \ll H$ (terminal-only) efficiency collapses as $1/H$.

**Evaluation.** Real and the primary lever on *efficiency* $B$ for agentic (multi-step) tasks. Note ScaleRL is a *single-turn reasoning* setting where $H$ is effectively the trace and reward is terminal-but-verifiable — so it under-samples this axis. **Verdict: real, first-order specifically for agents (vs. single-turn reasoning), the key $B$-mover.**

### Axis 4 — Verification fidelity $\nu \Rightarrow$ hard multiplier on $S_{\text{eff}}$, caps $A$

**Derivation.** RLVR's entire premise is that a *correct, cheap* verifier replaces a learned reward model, removing reward-hacking. But fidelity is not free. Agent-RLVR (arXiv:2506.11425) shows real SWE environments need engineered verifiable criteria (tests, execution) plus guidance to make RLVR work at all. The limits literature — "Does RL Really Incentivize Reasoning Capacity Beyond the Base Model?" (limit-of-rlvr.github.io; NeurIPS 2025) — shows RLVR sharpens the base model's existing distribution (raises pass@1) but can *shrink* pass@k, i.e. it exploits the verifier rather than expanding capability. If a fraction $(1-\nu)$ of rewards are wrong, gradient is actively poisoned:

$$
S_{\text{eff}} \propto \nu - \lambda(1-\nu), \qquad A \le A_{\text{oracle}}\cdot \nu^{\beta}.
$$

The $\lambda$ term captures that a *wrong* reward is worse than a *missing* one — it teaches the exploit.

**Evaluation.** Real, and the axis most prone to silent failure: verifier gaps become reward hacks that FLOPs amplify. Distinct from Axis 1 (diversity of *problems*) and Axis 3 (density of *signal*); this is *correctness of signal*. **Verdict: real, first-order, and a ceiling ($A$) constraint distinct from all others.**

### Axis 5 — Reset / replay $\rho \Rightarrow u(\rho)$, sets $B$ (with a cliff)

**Derivation.** Two coupled sub-levers. (a) **Replay ratio** — reusing rollouts raises data efficiency but induces *primacy bias*: Nikishin et al. (ICML 2022) show over-fitting to early data caps the agent, and periodic **weight resets** (while keeping the replay buffer) break the "replay-ratio barrier," letting you crank reuse without collapse. (b) LLM-specific: LoRR ("Sample-efficient LLM Optimization with Reset Replay," OpenReview) ports dataset-reset + model-reset to LLM RL for efficiency. ScaleRL's own "epochs over training prompts" *is* replay, and its "No-Positive-Resampling" / zero-variance filtering are replay-hygiene choices that moved efficiency. Model:

$$
u(\rho) = \rho^{\gamma}\quad(\gamma<1,\text{ diminishing}) \quad\text{until primacy-bias cliff at } \rho^\ast,\ \text{reset pushes } \rho^\ast \to \infty.
$$

**Evaluation.** Real but *efficiency-only* ($B$/$C_{\text{mid}}$) under ScaleRL's finding that off-policy/reuse choices don't move $A$. The reset half is the interesting, under-explored part for LLMs: it converts a hard ceiling (primacy bias) into a tunable knob. **Verdict: real, second-order for $A$, first-order for $B$; reset is the novel sub-lever.**

### Axis 6 — Exploration diversity $\sigma \Rightarrow c(\sigma)$, sets $A$ *and* $B$

**Derivation.** RL post-training exhibits **entropy collapse**: pass@1 climbs while pass@k falls (Kaddour, 2026; the RLVR-limits results), meaning the policy narrows onto one mode and *loses* the breadth needed for hard/OOD problems. DAPO's asymmetric clipping is adopted in ScaleRL precisely "to avoid entropy collapse and maintain output diversity." Diversity-aware policy optimization (NeurIPS 2025), dual-scale diversity regularization (arXiv:2602.19895), and sequential/history-aware sampling (OpenReview ifC1qxP5rS) all target the same failure: without maintained $\sigma$, generalization deteriorates OOD. Because collapse both lowers the reachable ceiling (can't solve modes you stopped sampling) and slows learning (no gradient on unseen solutions), $\sigma$ is dual-purpose:

$$
c(\sigma) = \sigma/\sigma^\ast \text{ (coverage of solution modes)},\quad A \propto \text{pass@k}(\sigma),\quad B \propto \dot{H}(\pi)\ \text{sustained}.
$$

**Evaluation.** Real and *dual-axis* (rare — touches both $A$ and $B$). It is the mechanism most easily conflated with Axis 1 (diversity of *inputs*) but is distinct: this is diversity of *outputs/behavior* on fixed inputs. The pass@1↑/pass@k↓ signature is the clean discriminator. **Verdict: real, first-order, dual-target, and the axis most often mismeasured (report pass@k, not pass@1).**

### Axis 7 — Compute / FLOPs $C$ — the carrier, not a source

**Derivation & evaluation.** ScaleRL shows $C$ is the *x-axis along which the other six express themselves*. FLOPs alone move you along a fixed sigmoid toward a fixed $A$; they do not raise $A$. This is the "bitter lesson with an asterisk": scale wins, but *only after* the ceiling-setting axes (diversity, verification, exploration) are fixed. **Verdict: necessary carrier, not an independent source of capability. Treat $C$ as the integration variable, the other six as the integrand.**

---

## 3. Which of the seven are actually independent?

| Axis | Primarily moves | Independent? | Confound to rule out |
|---|---|---|---|
| 1 Env diversity | $A$ | Yes (input space) | vs. exploration (output space) |
| 2 Horizon | $C_{\text{mid}}$ (+$A$ via 3) | **No** — effect on $A$ routed through Axis 3 | credit assignment |
| 3 Credit assignment | $B$, $C_{\text{mid}}$ | Yes | horizon length |
| 4 Verification fidelity | $A$ (hard cap) | Yes | reward-model quality vs. diversity |
| 5 Reset/replay | $B$ (cliff) | Partly — reset is independent, replay-ratio couples to compute | epochs = compute |
| 6 Exploration diversity | $A$ **and** $B$ | Yes | env diversity (input vs output) |
| 7 FLOPs | position on curve | Carrier | everything |

**Net independent capability sources: five** — diversity (1), credit assignment (3), verification (4), reset (5b), exploration (6). Horizon (2) is a *cost/mediator* not a source; FLOPs (7) is the *carrier*. This is the framework's sharpest independent claim and the thing the ablations below are built to test.

---

## 4. Empirical ablations to distinguish the axes

Design principle: **hold the ScaleRL sigmoid fitting protocol fixed** (fit on early ¼ of compute, extrapolate, validate at the ×-markers) and, for each axis, measure whether the intervention moves $A$, $B$, or $C_{\text{mid}}$. An axis is "real and independent" iff it moves a curve parameter that no other axis in the battery moves, under a shared confound control.

### A1 — Diversity vs. exploration disambiguation (input vs. output)
- **Manipulate:** $N_{\text{env}}$ (number of distinct prompts/levels) on one arm; $\sigma$ (entropy bonus / diversity reg) on the other; a $2\times2$ grid.
- **Measure:** $A$, and crucially **pass@1 vs pass@k gap** on held-out *and* OOD sets.
- **Discriminator:** Axis 1 raises $A$ on OOD *inputs*; Axis 6 raises pass@k *and* prevents the pass@1↑/pass@k↓ collapse on *fixed* inputs. If entropy reg raises OOD-input generalization with $N_{\text{env}}$ fixed, the two are confounded and must be co-modeled.

### A2 — Horizon × credit assignment (test the "Axis 2 not independent" claim)
- **Manipulate:** synthetic horizon $H \in \{1, 4, 16, 64\}$ (compose sub-tasks) × credit resolution $\phi \in \{\text{terminal}, \text{per-step verifiable}\}$.
- **Measure:** $C_{\text{mid}}(H)$ exponent $\alpha$; $A(H)$ under each $\phi$.
- **Prediction (falsifiable):** under terminal-only reward, $A$ *falls* with $H$; under per-step credit, $A$ is *flat* in $H$ and only $C_{\text{mid}}$ rises. If $A$ stays flat with terminal reward, Axis 2 *is* independently ceiling-setting and the framework is wrong.

### A3 — Verification fidelity poisoning curve
- **Manipulate:** inject label noise into the verifier at $\nu \in \{1.0, 0.95, 0.85, 0.7\}$ (flip a controlled fraction of pass/fail).
- **Measure:** $A(\nu)$; separately track a **held-out oracle-verified** eval to detect reward hacking (train-reward up, oracle-eval flat/down).
- **Discriminator:** fit $A \le A_{\text{oracle}}\nu^\beta$; the *sign* of the divergence between train-reward and oracle-eval isolates Axis 4 from Axis 1 (a diversity deficit lowers both together; a verifier deficit opens a gap).

### A4 — Reset breaks the replay cliff
- **Manipulate:** replay ratio $\rho \in \{1,4,16\}$ epochs × {no reset, periodic weight reset keeping buffer}.
- **Measure:** $B$ and the $\rho^\ast$ at which $A$ degrades (primacy bias).
- **Prediction:** without reset, $A$ degrades past $\rho^\ast$ (Nikishin); with reset, $B$ keeps rising and $A$ holds. Isolates the *reset* sub-lever (independent) from the *replay-ratio* sub-lever (couples to compute — control by equalizing total gradient steps, not epochs).

### A5 — Exploration collapse rescue
- **Manipulate:** {DAPO asym-clip only} vs {+diversity reg} vs {+history-aware sampling} on a fixed base + dataset.
- **Measure:** policy entropy trajectory $H(\pi_t)$, pass@k over $k\in\{1,8,64\}$, OOD transfer.
- **Discriminator:** the pass@1↑ / pass@k↓ signature is the operational definition of collapse; an intervention that flattens $H(\pi_t)$ decay *and* preserves pass@k is moving Axis 6, not Axis 1 (inputs unchanged).

### A6 — The binding-constraint (min-multiplier) test
- **Setup:** estimate each multiplier $\{d,\nu,g,u,c\}$ on a base run; identify the smallest.
- **Intervention:** add a fixed FLOP budget $\Delta C$ three ways — (i) pure compute, (ii) invest $\Delta C$-equivalent into the *smallest* multiplier, (iii) into a *non*-smallest multiplier.
- **Prediction (core claim):** (ii) > (i) ≈ (iii) in $\Delta A$. If pure compute (i) matches (ii), the resource-vector factorization is unnecessary and a FLOP-only law suffices — the null hypothesis this whole framework is built to reject.

### A7 — Cross-axis independence via factorial curve-fitting
- Run a fractional-factorial over all six non-compute axes (Plackett–Burman screening, then full grid on the survivors), fitting the ScaleRL sigmoid per cell.
- Regress $(A, B, C_{\text{mid}})$ on the six axes with interaction terms.
- **Independence verdict:** an axis is independent iff its main effect on some curve parameter is significant *after* partialling out the others; the horizon×credit interaction term should absorb Axis 2's apparent effect on $A$ (confirming §3).

---

## 5. Summary claims (what to bet on)

1. **The scaling object is a resource vector, not FLOPs.** $A,B,C_{\text{mid}}$ factorize across six non-compute axes; compute is the carrier.
2. **Ceiling-setters ($A$):** environment diversity, verification fidelity, exploration diversity. **Slope-setters ($B$):** credit assignment, reset/replay, exploration. **Midpoint ($C_{\text{mid}}$):** horizon, credit.
3. **Only five axes are independent capability sources.** Horizon is a cost mediator; FLOPs the carrier.
4. **The binding multiplier governs marginal return.** Scaling the smallest of $\{d,\nu,g,u,c\}$ beats scaling compute (A6 is the decisive experiment).
5. **The two most silently mismeasured axes** are verification (gap = reward hacking, hidden unless oracle-eval is tracked) and exploration (report pass@k, not pass@1).
6. **Biggest open holes in the literature** (per Lambert): $A(N_{\text{env}})$ — no data-regime scaling law yet — and base-model choice. These are exactly Axes 1 and the pre-RL prior.

---

## References (discovered via search, 2026-07-09)

- Khatri, Madaan, et al. *The Art of Scaling Reinforcement Learning Compute for LLMs* (ScaleRL). arXiv:2510.13786, 2025. — sigmoid compute law; $A/B/C_{\text{mid}}$; loss type & precision move $A$; curriculum/normalization/off-policy move $B$; multi-task & length as predictable axes; >400k GPU-hrs.
- Lambert, N. *How to scale RL.* interconnects.ai, 2025-10-20. — interpretation; bounded-metric caveat; open holes (data regime, base model); very-long-horizon RL.
- Cobbe et al. *Leveraging Procedural Generation to Benchmark RL* (Procgen). ICML 2020 (PMLR v119). — env count → generalization; sample-efficiency vs generalization split.
- Dennis et al. (PAIRED) and follow-ups; PLR; ACCEL; PACE (arXiv:2605.01358, 2026). — Unsupervised Environment Design; regret/learning-progress curricula.
- LessWrong, *The effect of horizon length on scaling laws*, 2023 (Procgen, Dota 2, MNIST). — horizon term in RL compute scaling.
- *Hindsight Credit Assignment for Long-Horizon LLM Agents.* arXiv:2603.08754, 2026. — sparse terminal reward credit in multi-step agents.
- *Intrinsic Credit Assignment for Long Horizon Interaction.* alphaXiv, 2026. — belief-as-intrinsic-reward.
- Microsoft Research. *Agent Lightning.* 2025. — per-call reward + credit-assignment module for agent RL.
- xxzcc, *Awesome-Credit-Assignment-in-LLM-RL* (GitHub). — corpus / problem framing.
- *Agent-RLVR: Training SWE Agents via RLVR.* arXiv:2506.11425, 2025. — verifiable criteria + guidance in real environments.
- *Limit of RLVR* (limit-of-rlvr.github.io); *Does RL Really Incentivize Reasoning Beyond the Base Model?* NeurIPS 2025. — RLVR sharpens base distribution; pass@k can shrink.
- Nikishin et al. *The Primacy Bias in Deep RL.* ICML 2022 (PMLR v162). — replay-ratio barrier; periodic resets.
- Kim et al. *Sample-Efficient and Safe Deep RL via Reset.* NeurIPS 2023. — reset methodology.
- *Sample-efficient LLM Optimization with Reset Replay* (LoRR). OpenReview. — dataset/model reset for LLM RL.
- Kaddour, J. *Causes of entropy collapse.* 2026. — pass@1↑/pass@k↓ signature.
- *Diversity-Aware Policy Optimization for LLM Reasoning.* NeurIPS 2025; *Dual-Scale Diversity Regularization* arXiv:2602.19895, 2026; *Enhancing Exploration via Sequential Sampling* OpenReview ifC1qxP5rS. — maintaining exploration; OOD generalization.
- Yu et al. *DAPO*, 2025; MiniMax *CISPO* (M1), 2025; Zheng et al. *GSPO*, 2025; Piche et al. *PipelineRL*, 2025. — loss/infra components underlying ScaleRL.
