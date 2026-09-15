<a id="math-wiki"></a>

# The Math Behind the Glossary

Companion to the LLM, self-play, and latent-reasoning glossary. Each entry gives
the defining equations, the shape of the objects, and the one line of intuition
that matters. This edition is a single-document wiki: concepts, symbols,
sections, and sources are addressable resources connected by typed links.

<a id="wiki-section-notation"></a>

## Notation and scope

- [$x$](#sym-x) $\in \mathbb{R}^{T \times d}$: a sequence of
  [$T$](#sym-t) token vectors, each of
  width $d$.
- [$\theta$](#sym-theta): model parameters.
- [$\pi_\theta(y \mid x)$](#sym-policy): the policy or language-model distribution over
  outputs $y$, conditioned on input $x$.
- [$L$](#sym-layers): number of layers; [$H$](#sym-heads): query heads;
  [$G$](#sym-kv-heads): key/value heads.
- Unless stated otherwise, vectors are row vectors and batch dimensions are
  omitted.

<a id="wiki-shape-ledger"></a>

### Shape ledger

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-notation">↑ section</a> ·
<a rel="prev" href="#math-wiki">← wiki home</a> ·
<a rel="next" href="#wiki-embeddings-unembedding-and-weight-tying">Embeddings, unembedding, and weight tying →</a> ·
<a rel="related" href="#wiki-concept-graph">concept graph</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

With the batch dimension restored, a decoder-only language model usually moves
the following objects through the network:

| Object | Typical shape | Meaning |
|---|---:|---|
| Token IDs | $B\times T$ | Integer vocabulary indices |
| Embedding table $E$ | $V\times d$ | One width-$d$ vector for each vocabulary item |
| Residual stream $h$ | $B\times T\times d$ | The main state carried between blocks |
| Queries $Q$ | $B\times H\times T\times d_k$ | One query per token and query head |
| Cached keys/values | $B\times G\times T\times d_k$ | Autoregressive attention memory |
| Attention scores | $B\times H\times T_q\times T_k$ | Pairwise query-key compatibility |
| Vocabulary logits | $B\times T\times V$ | Unnormalized next-token scores |

Here $B$ is batch size, $V$ vocabulary size, and usually
$Hd_k=d$ (though this is an architectural choice, not an identity). A notation
such as $x\in\mathbb{R}^{T\times d}$ refers to embedded vectors; raw token IDs
are discrete and live in $\{0,\ldots,V-1\}^T$.

**Intuition:** following the axes is often enough to find an incorrect equation
or implementation.

The formulas are reference models, not universal implementation contracts.
Constant factors, normalization axes, loss signs, and cache layouts vary across
papers and systems. Empirical scaling laws are fits over a measured regime, not
physical laws.

## Contents

1. [Hypermedia contract](#wiki-hypermedia-contract)
2. [Symbol wiki](#wiki-symbols)
3. [Concept graph](#wiki-concept-graph)
4. [Transformer foundations](#wiki-section-1)
5. [Pretraining and scaling](#wiki-section-2)
6. [Post-training and alignment](#wiki-section-3)
7. [Reasoning, test-time compute, and recurrent depth](#wiki-section-4)
8. [Self-play, search, and reinforcement learning](#wiki-section-5)
9. [Inference and sampling](#wiki-section-6)
10. [Evaluation](#wiki-section-7)
11. [Primary references](#wiki-sources)

<a id="wiki-hypermedia-contract"></a>

## Hypermedia contract

Every level-three entry is an independently addressable resource with a stable
fragment beginning <code>#wiki-</code>. Its navigation bar exposes typed
transitions:

| Relation | Meaning |
|---|---|
| <code>up</code> | Move to the entry's containing subject area |
| <code>prev</code> / <code>next</code> | Traverse the complete glossary without returning to an index |
| <code>related</code> | Move to the conceptual hub for the current subject |
| <code>index</code> | Return to the cross-domain concept graph |
| <code>help</code> | Resolve notation in the symbol wiki |

The prose immediately before or after an equation is part of that equation's
contract. It establishes object types and shapes, defines local symbols, states
important assumptions or caveats, and ends with the operative intuition.
Definitions in an entry override overloaded global symbols—for example, $r$
may mean reward, rank, or recurrence count locally.

An entry remains useful when reached directly through a fragment: the
navigation identifies its parent and neighbors, the symbol wiki resolves common
notation, and the related link provides a path back into the semantic graph.
This is the documentation analogue of HATEOAS: the current resource carries the
controls needed to discover valid next resources.

<a id="wiki-symbols"></a>

## Symbol wiki

| Symbol | Type or shape | Default meaning | Representative use |
|---|---|---|---|
| <a id="sym-batch"></a>$B$ | $\mathbb{N}$ | Batch size | [Shape ledger](#wiki-shape-ledger) |
| <a id="sym-t"></a>$T$ | $\mathbb{N}$ | Sequence or context length | [Attention](#wiki-scaled-dot-product-attention) |
| <a id="sym-vocab"></a>$V$ | $\mathbb{N}$ | Vocabulary size | [Embeddings](#wiki-embeddings-unembedding-and-weight-tying) |
| <a id="sym-width"></a>$d$ | $\mathbb{N}$ | Residual-stream width | [Transformer block](#wiki-pre-norm-transformer-block) |
| <a id="sym-head-width"></a>$d_k,d_v$ | $\mathbb{N}$ | Key/query and value head widths | [Attention](#wiki-scaled-dot-product-attention) |
| <a id="sym-layers"></a>$L$ | $\mathbb{N}$ | Number of model layers | [KV cache](#wiki-kv-cache) |
| <a id="sym-heads"></a>$H$ | $\mathbb{N}$ | Number of query heads | [Multi-head attention](#wiki-multi-head-grouped-query-and-multi-query-attention) |
| <a id="sym-kv-heads"></a>$G$ | $\mathbb{N}$ | Number of key/value heads | [GQA and MQA](#wiki-multi-head-grouped-query-and-multi-query-attention) |
| <a id="sym-parameters"></a>$N$ | $\mathbb{N}$ | Parameter count | [Compute accounting](#wiki-compute-accounting) |
| <a id="sym-data"></a>$D$ | $\mathbb{N}$ | Training-token count | [Scaling laws](#wiki-empirical-scaling-laws) |
| <a id="sym-x"></a>$x$ | tokens or $\mathbb{R}^{T\times d}$ | Input sequence; the entry states whether IDs or vectors | [Shape ledger](#wiki-shape-ledger) |
| <a id="sym-y"></a>$y$ | token sequence or model output | Completion, target, or transformed representation | [Sequence likelihood](#wiki-sequence-likelihood-and-length-normalization) |
| <a id="sym-hidden"></a>$h,s$ | model-dependent tensor | Hidden or recurrent state | [Recurrent depth](#wiki-standard-depth-versus-recurrent-depth) |
| <a id="sym-logits"></a>$\ell,z$ | $\mathbb{R}^{V}$ or score vector | Unnormalized scores; $z$ can also denote a latent trace locally | [Sampling](#wiki-temperature-top-k-top-p-and-min-p) |
| <a id="sym-theta"></a>$\theta$ | parameter pytree | Trainable model parameters | [AdamW](#wiki-adamw) |
| <a id="sym-policy"></a>$\pi_\theta(\cdot\mid x)$ | probability distribution | Model policy conditioned on $x$ | [RLHF](#wiki-kl-regularized-rlhf) |
| <a id="sym-probability"></a>$p,q$ | probability distributions or scalars | Target/model probabilities; entries identify direction | [KL directions](#wiki-forward-versus-reverse-kl) |
| <a id="sym-reward"></a>$r$ | $\mathbb{R}$, vector, or integer | Reward; also rank or recurrence count when declared | [RLVR](#wiki-rl-with-verifiable-rewards) |
| <a id="sym-value"></a>$V^\pi,Q^\pi,A^\pi$ | $\mathbb{R}$-valued functions | State value, action value, and advantage | [MDPs](#wiki-mdps-returns-and-bellman-equations) |
| <a id="sym-expectation"></a>$\mathbb{E}$ | operator | Expectation under the displayed distribution | [Policy gradient](#wiki-policy-gradient-and-generalized-advantage-estimation) |
| <a id="sym-kl"></a>$D_{\mathrm{KL}}(p\|q)$ | $\mathbb{R}_{\ge0}$ | Directed relative entropy | [KL directions](#wiki-forward-versus-reverse-kl) |
| <a id="sym-entropy"></a>$H(p)$ | $\mathbb{R}_{\ge0}$ | Shannon entropy | [Entropy regularization](#wiki-entropy-regularization) |
| <a id="sym-indicator"></a>$\mathbf{1}\{\cdot\}$ | $\{0,1\}$ | Indicator of a proposition | [Verifiable rewards](#wiki-rl-with-verifiable-rewards) |
| <a id="sym-norm"></a>$\|\cdot\|_2$ | $\mathbb{R}_{\ge0}$ | Euclidean norm unless another norm is named | [Gradient clipping](#wiki-learning-rate-schedules-and-clipping) |
| <a id="sym-hadamard"></a>$\odot$ | operator | Elementwise product | [SwiGLU](#wiki-pre-norm-transformer-block) |

<a id="wiki-concept-graph"></a>

## Concept graph

The arrows are semantic routes, not claims of strict implementation
dependence. Each node is a live fragment link.

**Representation and learning**

[token IDs](#wiki-embeddings-unembedding-and-weight-tying)
→ [attention](#wiki-scaled-dot-product-attention)
→ [multi-head attention](#wiki-multi-head-grouped-query-and-multi-query-attention)
→ [transformer block](#wiki-pre-norm-transformer-block)
→ [next-token loss](#wiki-next-token-objective)
→ [scaling laws](#wiki-empirical-scaling-laws)

**Efficient sequence models**

[stable softmax](#wiki-stable-softmax-and-its-sensitivity)
→ [FlashAttention](#wiki-flashattention-and-online-softmax)
→ [GQA / MQA](#wiki-multi-head-grouped-query-and-multi-query-attention)
→ [MLA](#wiki-multi-head-latent-attention)
→ [KV-cache accounting](#wiki-kv-cache)
→ [paged cache](#wiki-paged-kv-cache-management)

[state-space model](#wiki-selective-state-space-models)
↔ [convolution view](#wiki-state-space-recurrence-as-a-convolution)
→ [linear-time sequence processing](#wiki-compute-accounting)

**Alignment and preference learning**

[SFT](#wiki-supervised-fine-tuning)
→ [reward model](#wiki-bradley-terry-reward-model)
→ [KL-regularized RLHF](#wiki-kl-regularized-rlhf)
→ [PPO](#wiki-ppo)

[RLHF optimum](#wiki-kl-regularized-rlhf)
→ [DPO](#wiki-dpo-and-close-variants)
→ [iterative preference learning](#wiki-spin)

[policy gradient](#wiki-policy-gradient-and-generalized-advantage-estimation)
→ [GRPO](#wiki-grpo)
→ [verifiable rewards](#wiki-rl-with-verifiable-rewards)
→ [verifier group gradient](#wiki-verifier-based-group-gradient)

**Reasoning and adaptive compute**

[latent traces](#wiki-chain-of-thought-as-latent-variable-marginalization)
→ [self-consistency](#wiki-majority-vote-and-correlated-samples)
→ [test-time scaling](#wiki-test-time-compute-scaling-and-passk)

[recurrent depth](#wiki-standard-depth-versus-recurrent-depth)
→ [fixed points](#wiki-fixed-points-and-deep-equilibrium-models)
→ [contraction](#wiki-contraction-rates-and-stopping-error)
→ [adaptive halting](#wiki-adaptive-computation-time)

[continuous thought](#wiki-coconut)
↔ [pause tokens](#wiki-pause-tokens)
↔ [internalized CoT](#wiki-chain-of-thought-internalization)

**Search and self-play**

[Bellman equations](#wiki-mdps-returns-and-bellman-equations)
→ [PUCT](#wiki-mcts-with-puct)
→ [MCTS backup](#wiki-mcts-backup-and-value-perspective)
→ [AlphaZero](#wiki-alphazero-training)
→ [expert iteration](#wiki-expert-iteration-as-an-operator)

[non-transitivity](#wiki-non-transitivity-and-populations)
→ [regret minimization](#wiki-regret-mixtures-and-equilibrium)
→ [PSRO](#wiki-psro-and-empirical-games)

**Serving and evaluation**

[sampling](#wiki-temperature-top-k-top-p-and-min-p)
→ [speculative decoding](#wiki-speculative-decoding)
→ [bandwidth model](#wiki-arithmetic-intensity-and-bandwidth)
→ [quantization](#wiki-weight-and-kv-cache-quantization)

[pass@k](#wiki-passk)
→ [uncertainty](#wiki-sampling-uncertainty-and-confidence-intervals)
→ [calibration](#wiki-calibration)
→ [proper scores](#wiki-proper-scoring-rules)
→ [multiple-comparison control](#wiki-multiple-comparisons-and-benchmark-saturation)

<a id="wiki-section-1"></a>

## 1. Transformer foundations

<a id="wiki-embeddings-unembedding-and-weight-tying"></a>

### Embeddings, unembedding, and weight tying

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-1">↑ section</a> ·
<a rel="prev" href="#wiki-shape-ledger">← Shape ledger</a> ·
<a rel="next" href="#wiki-scaled-dot-product-attention">Scaled dot-product attention →</a> ·
<a rel="related" href="#wiki-scaled-dot-product-attention">attention hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For token ID $i_t$, the input vector is a row of the embedding table:

$$
h_t^{(0)}=E_{i_t,:},\qquad E\in\mathbb{R}^{V\times d}.
$$

After the final block and normalization, vocabulary logits are

$$
\ell_t=h_t^{(L)}W_U+b,\qquad
W_U\in\mathbb{R}^{d\times V}.
$$

Weight tying sets $W_U=E^\top$, saving $Vd$ parameters and placing input and
output tokens in the same learned geometry. Tying does not make the two uses
identical: the final hidden state, normalization, and optional output bias still
change the scores.

**Intuition:** an LM begins by looking up a token vector and ends by comparing
its hidden state against every possible token vector.

<a id="wiki-scaled-dot-product-attention"></a>

### Scaled dot-product attention

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-1">↑ section</a> ·
<a rel="prev" href="#wiki-embeddings-unembedding-and-weight-tying">← Embeddings, unembedding, and weight tying</a> ·
<a rel="next" href="#wiki-stable-softmax-and-its-sensitivity">Stable softmax and its sensitivity →</a> ·
<a rel="related" href="#wiki-concept-graph">concept graph</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For $Q \in \mathbb{R}^{T_q \times d_k}$,
$K \in \mathbb{R}^{T_k \times d_k}$, and
$V \in \mathbb{R}^{T_k \times d_v}$,

$$
\operatorname{Attn}(Q,K,V)
=
\operatorname{softmax}\!\left(\frac{QK^\top}{\sqrt{d_k}}+M\right)V.
$$

For self-attention,

$$
Q=xW_Q,\qquad K=xW_K,\qquad V=xW_V,
$$

with $W_Q,W_K \in \mathbb{R}^{d \times d_k}$ and
$W_V \in \mathbb{R}^{d \times d_v}$. A causal mask uses
$M_{ij}=-\infty$ when $j>i$. Scaling by $1/\sqrt{d_k}$ keeps the variance
of dot-product logits roughly constant as head width grows, which prevents
premature softmax saturation.

**Intuition:** each query forms a probability distribution over compatible
keys, then averages their values.

<a id="wiki-stable-softmax-and-its-sensitivity"></a>

### Stable softmax and its sensitivity

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-1">↑ section</a> ·
<a rel="prev" href="#wiki-scaled-dot-product-attention">← Scaled dot-product attention</a> ·
<a rel="next" href="#wiki-flashattention-and-online-softmax">FlashAttention and online softmax →</a> ·
<a rel="related" href="#wiki-scaled-dot-product-attention">attention hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For a score row $z\in\mathbb{R}^{T_k}$, numerically stable softmax subtracts its
maximum:

$$
p_i=\frac{\exp(z_i-m)}{\sum_j\exp(z_j-m)},\qquad m=\max_j z_j.
$$

Subtracting a common constant leaves softmax unchanged but prevents overflow.
Its Jacobian is

$$
\frac{\partial p_i}{\partial z_j}=p_i(\mathbf{1}[i=j]-p_j),
\qquad
J=\operatorname{diag}(p)-pp^\top.
$$

When one probability is almost one, most entries of $J$ are almost zero:
overconfident attention has weak gradients. Masks must be applied before the
row reduction; a finite “large negative” mask can leak probability in low
precision if it is not sufficiently negative.

**Intuition:** softmax is invariant to score offsets, but not to score scale;
very wide score gaps make both the distribution and its learning signal sharp.

<a id="wiki-flashattention-and-online-softmax"></a>

### FlashAttention and online softmax

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-1">↑ section</a> ·
<a rel="prev" href="#wiki-stable-softmax-and-its-sensitivity">← Stable softmax and its sensitivity</a> ·
<a rel="next" href="#wiki-multi-head-grouped-query-and-multi-query-attention">Multi-head, grouped-query, and multi-query attention →</a> ·
<a rel="related" href="#wiki-scaled-dot-product-attention">attention hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

FlashAttention computes the same exact attention result while tiling $Q$, $K$,
and $V$ so the full $T\times T$ score matrix is never written to high-bandwidth
memory. For successive blocks of scores, an online softmax keeps a running
maximum $m$, normalizer $\ell$, and weighted value accumulator $o$. Combining
an old block with a new score block $z$ gives

$$
m'=\max(m,\max z),
$$

$$
\ell'=e^{m-m'}\ell+\sum_j e^{z_j-m'},
$$

$$
o'=e^{m-m'}o+\sum_j e^{z_j-m'}v_j,
\qquad
\operatorname{Attn}=o'/\ell'.
$$

The rescaling factors correct earlier partial sums whenever a later block has a
larger maximum. Arithmetic remains quadratic in sequence length, but
intermediate storage and HBM traffic fall dramatically. “Flash” therefore
describes an IO-aware evaluation order, not an approximation to attention.

**Intuition:** fuse attention into small on-chip tiles and carry only the
statistics needed to normalize them exactly.

<a id="wiki-multi-head-grouped-query-and-multi-query-attention"></a>

### Multi-head, grouped-query, and multi-query attention

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-1">↑ section</a> ·
<a rel="prev" href="#wiki-flashattention-and-online-softmax">← FlashAttention and online softmax</a> ·
<a rel="next" href="#wiki-multi-head-latent-attention">Multi-head latent attention →</a> ·
<a rel="related" href="#wiki-scaled-dot-product-attention">attention hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For $H$ query heads,

$$
\operatorname{MHA}(x)
=
\operatorname{Concat}(h_1,\ldots,h_H)W_O,
$$

$$
h_i
=
\operatorname{Attn}
\left(xW_Q^{(i)},xW_K^{(i)},xW_V^{(i)}\right).
$$

In [grouped-query attention](#wiki-multi-head-grouped-query-and-multi-query-attention)
(GQA), $H$ query heads share $G<H$ key/value heads. Multi-query attention
(MQA) is the special case $G=1$. Ignoring batch, layer, and datatype
dimensions, [cached state](#wiki-kv-cache) per token falls from roughly
$2Hd_k$ elements to $2Gd_k$.

**Intuition:** keep diverse queries while sharing the memory they read from.

<a id="wiki-multi-head-latent-attention"></a>

### Multi-head latent attention

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-1">↑ section</a> ·
<a rel="prev" href="#wiki-multi-head-grouped-query-and-multi-query-attention">← Multi-head, grouped-query, and multi-query attention</a> ·
<a rel="next" href="#wiki-pre-norm-transformer-block">Pre-norm transformer block →</a> ·
<a rel="related" href="#wiki-scaled-dot-product-attention">attention hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Multi-head latent attention (MLA) compresses the key/value state into a smaller
latent:

$$
c_t^{KV}=x_tW^{DKV}\in\mathbb{R}^{d_c},
\qquad d_c\ll Hd_k,
$$

then reconstructs the content components,

$$
k_t^C=c_t^{KV}W^{UK},
\qquad
v_t^C=c_t^{KV}W^{UV}.
$$

The cache stores $c_t^{KV}$, plus any separate positional component required
by the implementation. In DeepSeek-V2-style MLA, the content up-projections can
be absorbed algebraically into the query and output projections at inference;
the decoupled [RoPE](#wiki-rotary-position-embedding) key is handled separately.
The result is a much smaller [KV cache](#wiki-kv-cache) without explicitly
materializing full per-head content keys and values.

**Intuition:** cache a shared low-rank memory and move reconstruction into
neighboring linear maps.

<a id="wiki-pre-norm-transformer-block"></a>

### Pre-norm transformer block

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-1">↑ section</a> ·
<a rel="prev" href="#wiki-multi-head-latent-attention">← Multi-head latent attention</a> ·
<a rel="next" href="#wiki-layernorm-rmsnorm-and-residual-placement">LayerNorm, RMSNorm, and residual placement →</a> ·
<a rel="related" href="#wiki-scaled-dot-product-attention">attention hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

$$
h' = h+\operatorname{MHA}(\operatorname{Norm}(h)),
\qquad
h''=h'+\operatorname{FFN}(\operatorname{Norm}(h')).
$$

A conventional feed-forward network is

$$
\operatorname{FFN}(u)=W_2\,\sigma(W_1u),
$$

while SwiGLU is commonly written

$$
\operatorname{SwiGLU}(u)
=W_2\left(\operatorname{Swish}(W_1u)\odot W_3u\right).
$$

RMSNorm is

$$
\operatorname{RMSNorm}(u)
=
\frac{u}{\sqrt{d^{-1}\sum_{i=1}^{d}u_i^2+\epsilon}}\odot g.
$$

**Intuition:** attention mixes information across positions, the FFN transforms
each position, and residual connections preserve a stable state highway. That
residual stream is the state
[recurrent-depth models](#wiki-standard-depth-versus-recurrent-depth) iterate
on.

<a id="wiki-layernorm-rmsnorm-and-residual-placement"></a>

### LayerNorm, RMSNorm, and residual placement

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-1">↑ section</a> ·
<a rel="prev" href="#wiki-pre-norm-transformer-block">← Pre-norm transformer block</a> ·
<a rel="next" href="#wiki-rotary-position-embedding">Rotary position embedding →</a> ·
<a rel="related" href="#wiki-scaled-dot-product-attention">attention hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

LayerNorm centers and scales each token across its feature axis:

$$
\operatorname{LayerNorm}(u)
=g\odot\frac{u-\mu}{\sqrt{\sigma^2+\epsilon}}+b,
\quad
\mu=\frac1d\sum_i u_i,
\quad
\sigma^2=\frac1d\sum_i(u_i-\mu)^2.
$$

RMSNorm removes the centering operation and usually the bias. Both normalize
per token, not across batch or sequence positions. In a pre-norm block,

$$
h_{\ell+1}=h_\ell+F_\ell(\operatorname{Norm}(h_\ell)),
$$

the residual path has an identity derivative term:

$$
\frac{\partial h_{\ell+1}}{\partial h_\ell}
=I+J_{F_\ell}J_{\operatorname{Norm}}.
$$

That direct path generally makes very deep optimization easier than post-norm,
where normalization also transforms the residual sum. Residual scaling,
initialization, and architecture still determine whether activations grow or
gradients become unstable.

**Intuition:** normalization controls feature scale; pre-norm also preserves an
unmodified route through which signals and gradients can cross many layers.

<a id="wiki-rotary-position-embedding"></a>

### Rotary position embedding

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-1">↑ section</a> ·
<a rel="prev" href="#wiki-layernorm-rmsnorm-and-residual-placement">← LayerNorm, RMSNorm, and residual placement</a> ·
<a rel="next" href="#wiki-alibi">ALiBi →</a> ·
<a rel="related" href="#wiki-scaled-dot-product-attention">attention hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For each coordinate pair $(2i,2i+1)$, rotate by a position-dependent angle.
With inverse frequency $\theta_i=b^{-2i/d_h}$ (commonly $b=10^4$, though modern
models use other bases),

$$
R_m^{(i)}=
\begin{bmatrix}
\cos(m\theta_i)&-\sin(m\theta_i)\\
\sin(m\theta_i)&\cos(m\theta_i)
\end{bmatrix},
\qquad
q'_m=R_mq_m,
\qquad
k'_n=R_nk_n.
$$

Orthogonality gives

$$
\langle R_mq,R_nk\rangle
=
\langle q,R_m^\top R_nk\rangle
=
\langle q,R_{n-m}k\rangle.
$$

Thus the attention score exposes relative displacement $n-m$, even though
the rotations are applied using absolute positions. Context-extension methods
such as NTK-aware scaling and YaRN modify the frequency schedule; they do not
make extrapolation free.

**Intuition:** encode position by rotating queries and keys so their dot product
depends on relative offset.

<a id="wiki-alibi"></a>

### ALiBi

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-1">↑ section</a> ·
<a rel="prev" href="#wiki-rotary-position-embedding">← Rotary position embedding</a> ·
<a rel="next" href="#wiki-mixture-of-experts">Mixture of experts →</a> ·
<a rel="related" href="#wiki-scaled-dot-product-attention">attention hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For causal attention in head $h$,

$$
\operatorname{softmax}_j\!\left(
\frac{q_i k_j^\top}{\sqrt{d_k}}-m_h(i-j)
\right),\qquad j\le i,
$$

where $m_h>0$ is a fixed per-head slope, usually chosen from a geometric
schedule.

**Intuition:** penalize distant keys directly in the attention logits, without
learned position embeddings.

<a id="wiki-mixture-of-experts"></a>

### Mixture of experts

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-1">↑ section</a> ·
<a rel="prev" href="#wiki-alibi">← ALiBi</a> ·
<a rel="next" href="#wiki-expert-capacity-overflow-and-router-stability">Expert capacity, overflow, and router stability →</a> ·
<a rel="related" href="#wiki-scaled-dot-product-attention">attention hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For $E$ experts, router logits are

$$
s=xW_r\in\mathbb{R}^{E}.
$$

For a top-$k$ set $\mathcal{T}=\operatorname{TopK}(s)$,

$$
g_e=
\frac{\exp(s_e)}{\sum_{e'\in\mathcal{T}}\exp(s_{e'})},
\qquad e\in\mathcal{T},
$$

$$
y=\sum_{e\in\mathcal{T}}g_e\operatorname{FFN}_e(x).
$$

A Switch-style load-balancing term is

$$
\mathcal{L}_{\text{aux}}
=
\alpha E\sum_{e=1}^{E}f_eP_e,
$$

where $f_e$ is the observed token fraction routed to expert $e$, and $P_e$
is its mean router probability. Roughly $k/E$ of the expert parameters are
active per token, but routers, shared layers,
[capacity limits](#wiki-expert-capacity-overflow-and-router-stability), and
communication still contribute compute and memory.

**Intuition:** buy parameter capacity without activating every parameter for
every token.

<a id="wiki-expert-capacity-overflow-and-router-stability"></a>

### Expert capacity, overflow, and router stability

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-1">↑ section</a> ·
<a rel="prev" href="#wiki-mixture-of-experts">← Mixture of experts</a> ·
<a rel="next" href="#wiki-selective-state-space-models">Selective state-space models →</a> ·
<a rel="related" href="#wiki-scaled-dot-product-attention">attention hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Sparse routing must allocate finite accelerator buffers. With $N_b$ tokens in a
routing batch, $E$ experts, and capacity factor $c$, a common per-expert capacity
is

$$
C_e=\left\lceil c\,\frac{kN_b}{E}\right\rceil.
$$

If more than $C_e$ assignments reach expert $e$, implementations must drop,
reroute, or pad tokens. Dropping silently replaces an expert contribution with
a residual or zero path; padding wastes compute. Besides the balancing loss,
some systems penalize large router logits with a z-loss,

$$
\mathcal{L}_z
=\lambda_z\left(\log\sum_{e=1}^{E}e^{s_e}\right)^2,
$$

which discourages runaway logit scales. Balanced token counts do not guarantee
balanced wall time: expert placement, all-to-all traffic, sequence packing, and
stragglers also matter.

**Intuition:** sparse experts save arithmetic only when the router distributes
both tokens and communication evenly enough to keep every device busy.

<a id="wiki-selective-state-space-models"></a>

### Selective state-space models

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-1">↑ section</a> ·
<a rel="prev" href="#wiki-expert-capacity-overflow-and-router-stability">← Expert capacity, overflow, and router stability</a> ·
<a rel="next" href="#wiki-state-space-recurrence-as-a-convolution">State-space recurrence as a convolution →</a> ·
<a rel="related" href="#wiki-scaled-dot-product-attention">attention hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

A linear continuous-time state-space model is

$$
\dot h(t)=Ah(t)+Bx(t),
\qquad
y(t)=Ch(t).
$$

Under a zero-order hold with step $\Delta$,

$$
\bar A=e^{\Delta A},
\qquad
\bar B=A^{-1}(e^{\Delta A}-I)B,
$$

or equivalently

$$
\bar B=(\Delta A)^{-1}(e^{\Delta A}-I)\Delta B,
$$

when the inverse notation is valid. Numerically stable implementations do not
need to form the inverse explicitly. The recurrence is

$$
h_t=\bar Ah_{t-1}+\bar Bx_t,
\qquad
y_t=Ch_t.
$$

Mamba makes $B_t$, $C_t$, and $\Delta_t$ input-dependent. This breaks the
[fixed convolution view](#wiki-state-space-recurrence-as-a-convolution) but
enables content-selective memory, linear-time recurrent decoding, and
parallel-scan training.

**Intuition:** attention retrieves from an explicit token memory; a selective
SSM continually compresses history into a state.

<a id="wiki-state-space-recurrence-as-a-convolution"></a>

### State-space recurrence as a convolution

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-1">↑ section</a> ·
<a rel="prev" href="#wiki-selective-state-space-models">← Selective state-space models</a> ·
<a rel="next" href="#wiki-compute-accounting">Compute accounting →</a> ·
<a rel="related" href="#wiki-scaled-dot-product-attention">attention hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For a time-invariant discrete SSM with $h_{-1}=0$, repeated substitution yields

$$
h_t=\sum_{j=0}^{t}\bar A^{\,t-j}\bar Bx_j,
$$

and therefore

$$
y_t=\sum_{j=0}^{t}C\bar A^{\,t-j}\bar Bx_j
=\sum_{j=0}^{t}K_{t-j}x_j,
\qquad
K_i=C\bar A^i\bar B.
$$

This duality permits recurrent evaluation in $O(T)$ sequential steps or
convolution/scan-based parallel training. Input-selective parameters make the
kernel depend on the data, so there is no single fixed convolution kernel; an
associative scan instead composes input-dependent affine transitions. Stability
is tied to the spectrum of $\bar A$: modes with magnitude below one decay,
while modes near one remember longer and modes above one can grow.

**Intuition:** a linear recurrence and a causal convolution are two evaluation
orders for the same fixed state-space system.

<a id="wiki-compute-accounting"></a>

### Compute accounting

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-1">↑ section</a> ·
<a rel="prev" href="#wiki-state-space-recurrence-as-a-convolution">← State-space recurrence as a convolution</a> ·
<a rel="next" href="#wiki-next-token-objective">Next-token objective →</a> ·
<a rel="related" href="#wiki-scaled-dot-product-attention">attention hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For a dense decoder-only transformer with $N$ non-embedding parameters and
$D$ training tokens, a common first-order estimate is

$$
C_{\text{train}}\approx 6ND,
$$

from approximately (2N) forward FLOPs and (4N) backward FLOPs per token.
Self-attention contributes a sequence-length-dependent term on the order of
$LdT^2$ per sequence; exact constants depend on which forward/backward and
projection operations are counted. It becomes material for long contexts and
cannot be dismissed by a universal threshold.

Dense autoregressive inference is approximately $2N$ arithmetic FLOPs per
new token, plus attention over [cached keys/values](#wiki-kv-cache) and memory
traffic. FLOPs are not latency:
[small-batch decode](#wiki-arithmetic-intensity-and-bandwidth) is commonly
bandwidth-bound.

**Intuition:** parameter matmuls dominate ordinary training, while cache traffic
and quadratic attention matter increasingly at long context.

<a id="wiki-section-2"></a>

## 2. Pretraining and scaling

<a id="wiki-next-token-objective"></a>

### Next-token objective

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-2">↑ section</a> ·
<a rel="prev" href="#wiki-compute-accounting">← Compute accounting</a> ·
<a rel="next" href="#wiki-cross-entropy-entropy-and-reducible-loss">Cross-entropy, entropy, and reducible loss →</a> ·
<a rel="related" href="#wiki-concept-graph">concept graph</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For $D$ tokens,

$$
\mathcal{L}(\theta)
=
-\frac{1}{D}\sum_{t=1}^{D}
\log\pi_\theta(x_t\mid x_{<t}),
\qquad
\operatorname{PPL}=e^{\mathcal{L}}.
$$

With hidden state $h_t\in\mathbb{R}^{d}$ and output embedding
$e_v\in\mathbb{R}^{d}$,

$$
\pi_\theta(x_t=v\mid x_{<t})
=
\frac{\exp(h_t^\top e_v)}
{\sum_{v'}\exp(h_t^\top e_{v'})}.
$$

[Input and output embeddings](#wiki-embeddings-unembedding-and-weight-tying)
are often tied.

**Intuition:** pretraining is multiclass classification at every token position.

<a id="wiki-cross-entropy-entropy-and-reducible-loss"></a>

### Cross-entropy, entropy, and reducible loss

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-2">↑ section</a> ·
<a rel="prev" href="#wiki-next-token-objective">← Next-token objective</a> ·
<a rel="next" href="#wiki-data-mixtures-and-reweighting">Data mixtures and reweighting →</a> ·
<a rel="related" href="#wiki-next-token-objective">pretraining hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Let $p(\cdot\mid c)$ be the true next-token distribution for context $c$, and
let $q_\theta$ be the model. The population cross-entropy decomposes as

$$
\mathbb{E}_{c}\,H(p,q_\theta)
=\mathbb{E}_{c}\left[
H(p)+D_{\mathrm{KL}}(p\|q_\theta)
\right].
$$

The conditional entropy $H(p)$ is irreducible uncertainty in the data source;
the KL term is the part an expressive, well-optimized model can reduce.
Perplexity is the exponential of average token cross-entropy, so it depends on
the tokenizer: changing token boundaries changes both the unit and the number
of predictions. Comparing perplexities across different tokenizers is therefore
usually invalid without conversion to a common unit such as bits per byte.

**Intuition:** loss combines uncertainty inherent in the corpus with error due
to the model, and tokenization decides the ruler used to measure both.

<a id="wiki-data-mixtures-and-reweighting"></a>

### Data mixtures and reweighting

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-2">↑ section</a> ·
<a rel="prev" href="#wiki-cross-entropy-entropy-and-reducible-loss">← Cross-entropy, entropy, and reducible loss</a> ·
<a rel="next" href="#wiki-empirical-scaling-laws">Empirical scaling laws →</a> ·
<a rel="related" href="#wiki-next-token-objective">pretraining hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Suppose data come from sources $p_j(x)$ sampled with mixture weights $w_j$:

$$
p_{\mathrm{train}}(x)=\sum_{j=1}^{J}w_jp_j(x),
\qquad
\sum_jw_j=1.
$$

The training objective is correspondingly

$$
\mathcal{L}_{\mathrm{mix}}(\theta)
=\sum_j w_j\,\mathbb{E}_{x\sim p_j}[\ell_\theta(x)].
$$

Upsampling a small high-quality source changes its objective weight, not its
information content; repeated examples become increasingly correlated. To
estimate a target distribution $p_*$ while sampling from proposal $q$, one can
use importance weights

$$
\mathbb{E}_{p_*}[\ell]
=\mathbb{E}_{q}\!\left[\frac{p_*(x)}{q(x)}\ell(x)\right],
$$

but large or poorly estimated ratios produce high variance. In practice,
mixture weights encode product priorities and data-quality judgments as much as
statistical correction.

**Intuition:** a corpus mixture is an explicit allocation of gradient budget
across domains.

<a id="wiki-empirical-scaling-laws"></a>

### Empirical scaling laws

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-2">↑ section</a> ·
<a rel="prev" href="#wiki-data-mixtures-and-reweighting">← Data mixtures and reweighting</a> ·
<a rel="next" href="#wiki-adamw">AdamW →</a> ·
<a rel="related" href="#wiki-next-token-objective">pretraining hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

One-variable Kaplan-style fits take the form

$$
L(N)\approx\left(\frac{N_c}{N}\right)^{\alpha_N},
\qquad
L(D)\approx\left(\frac{D_c}{D}\right)^{\alpha_D},
$$

with reported exponents around α_N = 0.076 and α_D = 0.095 for the
paper's setup. A Chinchilla-style joint fit is

$$
L(N,D)=E+\frac{A}{N^\alpha}+\frac{B}{D^\beta},
$$

with one reported fit near $\alpha=0.34$, $\beta=0.28$, and $E=1.69$. Under
$C=\kappa ND$, minimizing that fitted loss yields

$$
N_{\text{opt}}\propto C^{\beta/(\alpha+\beta)},
\qquad
D_{\text{opt}}\propto C^{\alpha/(\alpha+\beta)}.
$$

The exponents are therefore about 0.45 and 0.55 for those particular values,
not exactly one half. The widely quoted $D\approx20N$ tokens-per-parameter
rule is an empirical summary of the studied compute-optimal frontier; it does
not follow from the exponents alone and is not automatically
[deployment-optimal](#wiki-arithmetic-intensity-and-bandwidth).

To see the exponents, substitute $D=C/(\kappa N)$:

$$
L(N)=E+AN^{-\alpha}
+B\left(\frac{\kappa N}{C}\right)^\beta.
$$

At an interior optimum, differentiating with respect to $N$ balances the two
reducible terms:

$$
\alpha A N^{-\alpha}
=\beta B D^{-\beta}.
$$

Solving this balance together with $ND=C/\kappa$ gives the powers above.
Inference demand can move the lifecycle optimum toward a smaller model trained
on more tokens, because every future generated token pays for active model
parameters again.

**Intuition:** at fixed training compute, an oversized undertrained model wastes
capacity; balance parameters and data for the objective and lifecycle you care
about.

<a id="wiki-adamw"></a>

### AdamW

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-2">↑ section</a> ·
<a rel="prev" href="#wiki-empirical-scaling-laws">← Empirical scaling laws</a> ·
<a rel="next" href="#wiki-muon">Muon →</a> ·
<a rel="related" href="#wiki-next-token-objective">pretraining hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For gradient $g_t$,

$$
m_t=\beta_1m_{t-1}+(1-\beta_1)g_t,
\qquad
v_t=\beta_2v_{t-1}+(1-\beta_2)g_t^2,
$$

$$
\hat m_t=\frac{m_t}{1-\beta_1^t},
\qquad
\hat v_t=\frac{v_t}{1-\beta_2^t},
$$

$$
\theta_{t+1}
=
\theta_t
-\eta\frac{\hat m_t}{\sqrt{\hat v_t}+\epsilon}
-\eta\lambda\theta_t.
$$

Values such as $\beta_1=0.9$, $\beta_2=0.95$, and weight decay $0.1$ are common
in LLM recipes, but are not universal defaults; they interact with the
[learning-rate schedule and clipping](#wiki-learning-rate-schedules-and-clipping).

**Intuition:** normalize momentum by a running estimate of coordinate-wise
gradient scale, then apply decoupled weight decay.

<a id="wiki-muon"></a>

### Muon

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-2">↑ section</a> ·
<a rel="prev" href="#wiki-adamw">← AdamW</a> ·
<a rel="next" href="#wiki-learning-rate-schedules-and-clipping">Learning-rate schedules and clipping →</a> ·
<a rel="related" href="#wiki-next-token-objective">pretraining hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For a two-dimensional weight matrix, form a momentum-like update $M_t$, then
approximately map its singular values toward one:

$$
M_t=U\Sigma V^\top,
\qquad
\operatorname{Orth}(M_t)=UV^\top.
$$

For a tall full-rank matrix this is

$$
\operatorname{Orth}(M_t)
=
M_t(M_t^\top M_t)^{-1/2};
$$

for a wide matrix, use the corresponding left-sided form. Muon approximates
this operation with Newton-Schulz iterations, then applies a scaled update.

$$
\theta_{t+1}=\theta_t-\eta U_t.
$$

**Intuition:** reduce domination by a few large singular directions in matrix
updates.

<a id="wiki-learning-rate-schedules-and-clipping"></a>

### Learning-rate schedules and clipping

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-2">↑ section</a> ·
<a rel="prev" href="#wiki-muon">← Muon</a> ·
<a rel="next" href="#wiki-parallelism-memory">Parallelism memory →</a> ·
<a rel="related" href="#wiki-next-token-objective">pretraining hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

With warmup length $T_w$, a cosine schedule is

$$
\eta_t=\eta_{\max}\frac{t}{T_w},\qquad t<T_w,
$$

then

$$
\eta_t
=
\eta_{\min}
+\frac{1}{2}(\eta_{\max}-\eta_{\min})
\left[1+\cos\!\left(\pi\frac{t-T_w}{T-T_w}\right)\right].
$$

Warmup-stable-decay (WSD) instead uses warmup, a long constant phase, and a
terminal decay. Global norm clipping at threshold $c$ is

$$
g\leftarrow g\min\!\left(1,\frac{c}{\lVert g\rVert_2}\right).
$$

**Intuition:** warmup avoids destructive early steps; clipping makes rare large
steps bounded.

<a id="wiki-parallelism-memory"></a>

### Parallelism memory

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-2">↑ section</a> ·
<a rel="prev" href="#wiki-learning-rate-schedules-and-clipping">← Learning-rate schedules and clipping</a> ·
<a rel="next" href="#wiki-data-tensor-pipeline-and-sequence-parallelism">Data, tensor, pipeline, and sequence parallelism →</a> ·
<a rel="related" href="#wiki-next-token-objective">pretraining hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

A common mixed-precision Adam accounting per parameter is

$$
2\text{ B (bf16 weight)}
+2\text{ B (bf16 gradient)}
+4\text{ B (fp32 master weight)}
+8\text{ B (fp32 }m,v\text{)}
=16\text{ B}.
$$

If all of that state is fully sharded across $P$ devices, the idealized share
is $16N/P$ bytes per device. Activations, temporary all-gather buffers,
fragmentation, embeddings, and unsharded state come on top.

**Intuition:** optimizer state, not just weights, sets the training-memory floor.

<a id="wiki-data-tensor-pipeline-and-sequence-parallelism"></a>

### Data, tensor, pipeline, and sequence parallelism

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-2">↑ section</a> ·
<a rel="prev" href="#wiki-parallelism-memory">← Parallelism memory</a> ·
<a rel="next" href="#wiki-supervised-fine-tuning">Supervised fine-tuning →</a> ·
<a rel="related" href="#wiki-next-token-objective">pretraining hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Parallelism divides different axes of the computation:

- Data parallelism replicates parameters and splits the batch; gradients need an
  all-reduce or reduce-scatter.
- Tensor parallelism shards matrix dimensions; activations communicate within
  each layer.
- Pipeline parallelism assigns layer ranges to stages; microbatches reduce the
  idle “bubble.”
- Sequence/context parallelism shards the token axis; long-context attention
  must exchange key/value or partial-attention statistics.

For $p$ pipeline stages and $m$ microbatches under a simple one-forward,
one-backward schedule, a rough ideal utilization is

$$
U_{\mathrm{pipe}}\approx\frac{m}{m+p-1}.
$$

This ignores imbalance and communication but exposes why too few microbatches
leave stages idle. If global batch size is $B_g$, data-parallel degree $P_d$,
and gradient accumulation is $a$, the local microbatch is

$$
B_{\mathrm{micro}}=\frac{B_g}{P_da}.
$$

Sharding lowers memory per device while adding collectives; the best layout is
the one whose communication fits beneath useful compute on the actual topology.

**Intuition:** distributed training exchanges memory pressure for synchronization,
and each parallelism strategy chooses a different tensor axis on which to make
that trade.

<a id="wiki-section-3"></a>

## 3. Post-training and alignment

<a id="wiki-supervised-fine-tuning"></a>

### Supervised fine-tuning

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-3">↑ section</a> ·
<a rel="prev" href="#wiki-data-tensor-pipeline-and-sequence-parallelism">← Data, tensor, pipeline, and sequence parallelism</a> ·
<a rel="next" href="#wiki-sequence-likelihood-and-length-normalization">Sequence likelihood and length normalization →</a> ·
<a rel="related" href="#wiki-kl-regularized-rlhf">alignment hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For demonstrations $(x,y)\sim\mathcal{D}$,

$$
\mathcal{L}_{\mathrm{SFT}}
=
-\mathbb{E}_{(x,y)\sim\mathcal{D}}
\sum_{t=1}^{|y|}
\log\pi_\theta(y_t\mid x,y_{<t}).
$$

Prompt tokens are normally masked from the loss when the goal is to learn only
the [response sequence likelihood](#wiki-sequence-likelihood-and-length-normalization)
conditional on the prompt.

**Intuition:** imitate target completions with ordinary next-token likelihood.

<a id="wiki-sequence-likelihood-and-length-normalization"></a>

### Sequence likelihood and length normalization

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-3">↑ section</a> ·
<a rel="prev" href="#wiki-supervised-fine-tuning">← Supervised fine-tuning</a> ·
<a rel="next" href="#wiki-bradley-terry-reward-model">Bradley-Terry reward model →</a> ·
<a rel="related" href="#wiki-kl-regularized-rlhf">alignment hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Autoregressive sequence probability factorizes as

$$
\log\pi_\theta(y\mid x)
=\sum_{t=1}^{|y|}
\log\pi_\theta(y_t\mid x,y_{<t}).
$$

Because each log-probability is nonpositive, raw sequence log-likelihood
systematically becomes more negative as responses grow. Ranking candidates by

$$
s_\alpha(y)
=\frac{\log\pi_\theta(y\mid x)}{|y|^\alpha}
$$

changes that bias, but it also changes the objective: $\alpha=0$ compares total
probability, while $\alpha=1$ compares average per-token log-probability.
Token-level averaging gives every response similar total weight; summing gives
long responses more gradient terms. EOS is part of the model and must be
included consistently when probabilities of variable-length sequences are
compared.

**Intuition:** “normalize by length” is not bookkeeping—it determines whether
the optimizer values whole sequences or average token quality.

<a id="wiki-bradley-terry-reward-model"></a>

### Bradley-Terry reward model

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-3">↑ section</a> ·
<a rel="prev" href="#wiki-sequence-likelihood-and-length-normalization">← Sequence likelihood and length normalization</a> ·
<a rel="next" href="#wiki-kl-regularized-rlhf">KL-regularized RLHF →</a> ·
<a rel="related" href="#wiki-kl-regularized-rlhf">alignment hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For a preferred response $y_w$ and rejected response $y_l$,

$$
\Pr(y_w\succ y_l\mid x)
=
\sigma\!\left(
r_\phi(x,y_w)-r_\phi(x,y_l)
\right),
$$

$$
\mathcal{L}_{\mathrm{RM}}
=
-\mathbb{E}
\log\sigma\!\left(
r_\phi(x,y_w)-r_\phi(x,y_l)
\right).
$$

Only reward differences are identified: adding the same prompt-dependent
constant to both rewards changes nothing. This same cancellation underlies
[DPO](#wiki-dpo-and-close-variants).

**Intuition:** learn a scalar score whose pairwise differences explain observed
preferences.

<a id="wiki-kl-regularized-rlhf"></a>

### KL-regularized RLHF

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-3">↑ section</a> ·
<a rel="prev" href="#wiki-bradley-terry-reward-model">← Bradley-Terry reward model</a> ·
<a rel="next" href="#wiki-forward-versus-reverse-kl">Forward versus reverse KL →</a> ·
<a rel="related" href="#wiki-concept-graph">concept graph</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

The sequence-level objective is

$$
\max_\pi\;
\mathbb{E}_{x\sim\mathcal{D},\,y\sim\pi(\cdot\mid x)}
[r_\phi(x,y)]
-\beta\,
\mathbb{E}_x
D_{\mathrm{KL}}\!\left(
\pi(\cdot\mid x)\,\|\,\pi_{\mathrm{ref}}(\cdot\mid x)
\right).
$$

If the policy can be optimized independently for each prompt and response,
the optimum has the Boltzmann form

$$
\pi^*(y\mid x)
=
\frac{1}{Z(x)}
\pi_{\mathrm{ref}}(y\mid x)
\exp\!\left(\frac{r(x,y)}{\beta}\right).
$$

For a sampled token, the log-ratio

$$
\log\pi_\theta(y_t\mid\cdot)
-\log\pi_{\mathrm{ref}}(y_t\mid\cdot)
$$

is an unbiased Monte Carlo contribution to the
[reverse-direction KL](#wiki-forward-versus-reverse-kl)
$D_{\mathrm{KL}}(\pi_\theta\|\pi_{\mathrm{ref}})$ under
$y_t\sim\pi_\theta$. A nonnegative low-variance estimator often used in GRPO
code is

$$
k_3(\rho)=(\rho-1)-\log\rho,
\qquad
\rho=\frac{\pi_{\mathrm{ref}}}{\pi_\theta}.
$$

**Intuition:** improve reward while paying for distributional movement away
from a reference behavior.

<a id="wiki-forward-versus-reverse-kl"></a>

### Forward versus reverse KL

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-3">↑ section</a> ·
<a rel="prev" href="#wiki-kl-regularized-rlhf">← KL-regularized RLHF</a> ·
<a rel="next" href="#wiki-policy-gradient-and-generalized-advantage-estimation">Policy gradient and generalized advantage estimation →</a> ·
<a rel="related" href="#wiki-kl-regularized-rlhf">alignment hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

The two directions penalize different failures:

$$
D_{\mathrm{KL}}(p\|q)
=\mathbb{E}_{p}\!\left[\log\frac{p}{q}\right],
\qquad
D_{\mathrm{KL}}(q\|p)
=\mathbb{E}_{q}\!\left[\log\frac{q}{p}\right].
$$

Forward KL is infinite if $q$ assigns zero probability to an event that $p$
can produce, so its optimum tends to cover all modes of $p$. Reverse KL
penalizes samples drawn from $q$ that fall in low-density regions of $p$, so a
restricted $q$ may concentrate on one convenient mode. In language models all
softmax probabilities are formally nonzero, but finite precision and enormous
ratios retain the practical distinction.

**Intuition:** forward KL fears missing target behavior; reverse KL fears
generating behavior the target dislikes.

<a id="wiki-policy-gradient-and-generalized-advantage-estimation"></a>

### Policy gradient and generalized advantage estimation

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-3">↑ section</a> ·
<a rel="prev" href="#wiki-forward-versus-reverse-kl">← Forward versus reverse KL</a> ·
<a rel="next" href="#wiki-ppo">PPO →</a> ·
<a rel="related" href="#wiki-kl-regularized-rlhf">alignment hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For a trajectory $\tau$,

$$
\nabla_\theta J
=
\mathbb{E}_{\tau\sim\pi_\theta}
\left[
\sum_t
\nabla_\theta\log\pi_\theta(a_t\mid s_t)A_t
\right].
$$

A baseline that does not depend on the sampled action leaves the expectation
unbiased while reducing variance. Generalized advantage estimation, used by
[PPO](#wiki-ppo), uses

$$
\hat A_t^{\mathrm{GAE}(\gamma,\lambda)}
=
\sum_{l=0}^{\infty}(\gamma\lambda)^l\delta_{t+l},
$$

$$
\delta_t=r_t+\gamma V(s_{t+1})-V(s_t).
$$

**Intuition:** reinforce sampled actions in proportion to how much better they
were than the baseline expectation.

<a id="wiki-ppo"></a>

### PPO

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-3">↑ section</a> ·
<a rel="prev" href="#wiki-policy-gradient-and-generalized-advantage-estimation">← Policy gradient and generalized advantage estimation</a> ·
<a rel="next" href="#wiki-off-policy-correction-and-effective-sample-size">Off-policy correction and effective sample size →</a> ·
<a rel="related" href="#wiki-kl-regularized-rlhf">alignment hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

With

$$
\rho_t(\theta)
=
\frac{\pi_\theta(a_t\mid s_t)}
{\pi_{\theta_{\mathrm{old}}}(a_t\mid s_t)},
$$

the clipped policy objective is

$$
J_{\mathrm{clip}}(\theta)
=
\mathbb{E}_t\!\left[
\min\!\left(
\rho_t\hat A_t,\,
\operatorname{clip}(\rho_t,1-\epsilon,1+\epsilon)\hat A_t
\right)
\right].
$$

A common combined maximization objective is

$$
J_{\mathrm{PPO}}
=
J_{\mathrm{clip}}
-c_1\mathbb{E}_t[(V_\theta(s_t)-R_t)^2]
+c_2\mathbb{E}_t[H(\pi_\theta(\cdot\mid s_t))].
$$

Implementations that minimize a loss negate the policy and entropy terms.
For an LLM, $s_t=(x,y_{<t})$ and $a_t=y_t$; a sequence reward is commonly
placed at the last generated token, with per-token KL shaping.

**Intuition:** reuse on-policy samples for several updates, while clipping
likelihood-ratio changes that would move the policy too far.

<a id="wiki-off-policy-correction-and-effective-sample-size"></a>

### Off-policy correction and effective sample size

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-3">↑ section</a> ·
<a rel="prev" href="#wiki-ppo">← PPO</a> ·
<a rel="next" href="#wiki-dpo-and-close-variants">DPO and close variants →</a> ·
<a rel="related" href="#wiki-kl-regularized-rlhf">alignment hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

If samples come from behavior policy $\mu$ but estimate an expectation under
$\pi$, importance sampling uses

$$
w(\tau)=\frac{\pi(\tau)}{\mu(\tau)}
=\prod_t\frac{\pi(a_t\mid s_t)}{\mu(a_t\mid s_t)}.
$$

The product can explode or vanish exponentially with trajectory length. For
normalized nonnegative weights $\tilde w_i=w_i/\sum_jw_j$, a common diagnostic
is

$$
\operatorname{ESS}
=\frac{1}{\sum_i\tilde w_i^2}
=\frac{(\sum_iw_i)^2}{\sum_iw_i^2}.
$$

ESS ranges from one to the sample count; a small value means a few trajectories
dominate. PPO-style clipping reduces variance but introduces bias. Stale
rollouts therefore cannot be made harmless merely by recording old-policy
log-probabilities: policy lag, long completions, and repeated optimizer epochs
all widen the ratio distribution.

**Intuition:** off-policy data are useful only to the extent that the new
policy still assigns them comparable probability.

<a id="wiki-dpo-and-close-variants"></a>

### DPO and close variants

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-3">↑ section</a> ·
<a rel="prev" href="#wiki-off-policy-correction-and-effective-sample-size">← Off-policy correction and effective sample size</a> ·
<a rel="next" href="#wiki-grpo">GRPO →</a> ·
<a rel="related" href="#wiki-kl-regularized-rlhf">alignment hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Define the reference-relative preference logit

$$
h_\theta(x,y_w,y_l)
=
\log\frac{\pi_\theta(y_w\mid x)}{\pi_{\mathrm{ref}}(y_w\mid x)}
-\log\frac{\pi_\theta(y_l\mid x)}{\pi_{\mathrm{ref}}(y_l\mid x)}.
$$

Direct preference optimization minimizes

$$
\mathcal{L}_{\mathrm{DPO}}
=
-\mathbb{E}
\left[
\log\sigma\!\left(\beta h_\theta(x,y_w,y_l)\right)
\right].
$$

The associated implicit reward is identifiable up to a prompt-dependent
constant:

$$
\hat r_\theta(x,y)
=
\beta\log
\frac{\pi_\theta(y\mid x)}{\pi_{\mathrm{ref}}(y\mid x)}
+\beta\log Z(x).
$$

Because $Z(x)$ cancels in pairwise differences, it need not be computed.
Related objectives include:

- IPO: $\mathcal{L}_{\mathrm{IPO}}
  =\mathbb{E}[(h_\theta-1/(2\beta))^2]$ under its standard convention.
- KTO: unpaired desirable/undesirable labels with a prospect-theoretic value
  function.
- SimPO: a reference-free, length-normalized score
  $s_\theta(x,y)=\frac{\beta}{|y|}\log\pi_\theta(y\mid x)$ and loss
  $-\log\sigma(s_w-s_l-\gamma)$.

**Intuition:** DPO turns the closed-form KL-regularized optimum into a binary
classification loss on preferred and rejected responses.

<a id="wiki-grpo"></a>

### GRPO

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-3">↑ section</a> ·
<a rel="prev" href="#wiki-dpo-and-close-variants">← DPO and close variants</a> ·
<a rel="next" href="#wiki-rl-with-verifiable-rewards">RL with verifiable rewards →</a> ·
<a rel="related" href="#wiki-kl-regularized-rlhf">alignment hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For prompt $x$, sample a group
$\{y_1,\ldots,y_G\}\sim\pi_{\theta_{\mathrm{old}}}$ with rewards $r_i$. The
original group-standardized advantage is

$$
\hat A_i
=
\frac{r_i-\bar r}{s_r+\varepsilon},
\qquad
\bar r=\frac{1}{G}\sum_{j=1}^{G}r_j.
$$

With token ratio

$$
\rho_{i,t}
=
\frac{\pi_\theta(y_{i,t}\mid x,y_{i,<t})}
{\pi_{\theta_{\mathrm{old}}}(y_{i,t}\mid x,y_{i,<t})},
$$

a representative maximization objective is

$$
J_{\mathrm{GRPO}}
=
\frac{1}{G}\sum_{i=1}^{G}\frac{1}{|y_i|}
\sum_{t=1}^{|y_i|}
\left[
\min\!\left(
\rho_{i,t}\hat A_i,\,
\operatorname{clip}(\rho_{i,t},1-\epsilon,1+\epsilon)\hat A_i
\right)
-\beta k_3(\rho^{\mathrm{ref}}_{i,t})
\right].
$$

[GRPO](#wiki-grpo) has no learned critic, and every token in a completion
shares its sequence-level advantage. Later variants change more than one
normalizer:

- Dr. GRPO removes reward standard-deviation scaling and replaces
  per-response length normalization with a fixed global divisor.
- DAPO uses asymmetric clipping, dynamic sampling, token-level loss
  aggregation, and other stability changes.

These changes target question-difficulty bias, response-length bias, and
training instability; they are not merely cosmetic rewrites of the same loss.

**Intuition:** compare sibling completions for the same prompt instead of
learning a separate value network.

<a id="wiki-rl-with-verifiable-rewards"></a>

### RL with verifiable rewards

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-3">↑ section</a> ·
<a rel="prev" href="#wiki-grpo">← GRPO</a> ·
<a rel="next" href="#wiki-outcome-rewards-process-rewards-and-verifier-noise">Outcome rewards, process rewards, and verifier noise →</a> ·
<a rel="related" href="#wiki-kl-regularized-rlhf">alignment hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For a deterministic checker,

$$
r(x,y)=\mathbf{1}\{\operatorname{verify}(x,y)=\mathrm{pass}\},
$$

possibly plus a format or partial-credit term. The prompt pass rate is

$$
p(x)=\mathbb{E}_{y\sim\pi}[r(x,y)].
$$

For Bernoulli rewards, reward variance is $p(1-p)$, largest at $p=1/2$. With
$G$ independent samples, the probability of seeing both outcomes is

$$
\Pr(0<C<G)
=
1-p^G-(1-p)^G,
\qquad C\sim\operatorname{Binomial}(G,p).
$$

If every sample in a group receives the same reward, its
[mean-centered GRPO gradient](#wiki-verifier-based-group-gradient) is zero.
This is why dynamic sampling or difficulty filtering removes prompts that are
currently always solved or always failed.

**Intuition:** verifiers eliminate reward-model ambiguity, but useful learning
still requires outcome variation.

<a id="wiki-outcome-rewards-process-rewards-and-verifier-noise"></a>

### Outcome rewards, process rewards, and verifier noise

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-3">↑ section</a> ·
<a rel="prev" href="#wiki-rl-with-verifiable-rewards">← RL with verifiable rewards</a> ·
<a rel="next" href="#wiki-best-of-n-and-its-kl-cost">Best-of-N and its KL cost →</a> ·
<a rel="related" href="#wiki-kl-regularized-rlhf">alignment hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

An outcome reward places all correctness information at the end:

$$
R(\tau)=r_T.
$$

A process reward model supplies intermediate scores $r_t$, giving return

$$
G_t=\sum_{j=t}^{T}\gamma^{j-t}r_j.
$$

This can improve credit assignment, but only if intermediate labels identify
valid reasoning rather than a preferred writing style. For a binary verifier
with false-positive rate $\alpha$ and false-negative rate $\beta$, observed
pass probability is

$$
\tilde p
=\Pr(\tilde r=1)
=\alpha+(1-\alpha-\beta)p,
$$

where $p$ is the true pass probability. If $\alpha+\beta<1$, the signal is
attenuated by $1-\alpha-\beta$; systematic false positives are especially
dangerous because optimization actively searches for them.

**Intuition:** dense feedback shortens the credit-assignment path, but a flawed
judge also creates more surfaces for reward hacking.

<a id="wiki-best-of-n-and-its-kl-cost"></a>

### Best-of-N and its KL cost

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-3">↑ section</a> ·
<a rel="prev" href="#wiki-outcome-rewards-process-rewards-and-verifier-noise">← Outcome rewards, process rewards, and verifier noise</a> ·
<a rel="next" href="#wiki-rejection-sampling-fine-tuning">Rejection-sampling fine-tuning →</a> ·
<a rel="related" href="#wiki-kl-regularized-rlhf">alignment hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Draw $N$ i.i.d. samples from $\pi_{\mathrm{ref}}$ and return the highest-reward
one. The induced policy obeys

$$
D_{\mathrm{KL}}(\pi_{\mathrm{BoN}}\|\pi_{\mathrm{ref}})
\le
\log N-\frac{N-1}{N}.
$$

This expression is an upper bound in the discrete-response setting, not a
universal equality. If rewards are approximately Gaussian,
$R_i\sim\mathcal{N}(\mu,\sigma_r^2)$, then to leading order

$$
\mathbb{E}\max_{i\le N}R_i
\approx
\mu+\sigma_r\sqrt{2\log N},
$$

with lower-order corrections. A proxy reward may keep increasing after true
quality plateaus or falls, which is the reward-overoptimization regime.

**Intuition:** more samples improve selection only logarithmically while moving
the selected-output distribution farther from the base policy.

<a id="wiki-rejection-sampling-fine-tuning"></a>

### Rejection-sampling fine-tuning

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-3">↑ section</a> ·
<a rel="prev" href="#wiki-best-of-n-and-its-kl-cost">← Best-of-N and its KL cost</a> ·
<a rel="next" href="#wiki-distillation">Distillation →</a> ·
<a rel="related" href="#wiki-kl-regularized-rlhf">alignment hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

$$
\mathcal{D}_+
=
\left\{
(x,y):
y\sim\pi_\theta(\cdot\mid x),\;
r(x,y)\ge\tau
\right\},
$$

followed by SFT on $\mathcal{D}_+$. This is an expert-iteration step whose
improvement operator is thresholded selection.

**Intuition:** generate broadly, retain high-scoring trajectories, then imitate
them.

<a id="wiki-distillation"></a>

### Distillation

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-3">↑ section</a> ·
<a rel="prev" href="#wiki-rejection-sampling-fine-tuning">← Rejection-sampling fine-tuning</a> ·
<a rel="next" href="#wiki-lora">LoRA →</a> ·
<a rel="related" href="#wiki-kl-regularized-rlhf">alignment hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Token-level forward-KL distillation is

$$
\mathcal{L}_{\mathrm{KD}}
=
\sum_t
D_{\mathrm{KL}}
\left(
\pi_T(\cdot\mid y_{<t})
\|
\pi_S(\cdot\mid y_{<t})
\right).
$$

Sequence-level distillation trains the student on teacher-generated samples.
[Reverse KL](#wiki-forward-versus-reverse-kl),
$D_{\mathrm{KL}}(\pi_S\|\pi_T)$, is mode-seeking and appears in on-policy
distillation, where the student learns from prefixes it actually visits.

**Intuition:** forward KL asks the student to cover the teacher's distribution;
reverse KL concentrates on teacher-supported modes reachable by the student.

<a id="wiki-lora"></a>

### LoRA

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-3">↑ section</a> ·
<a rel="prev" href="#wiki-distillation">← Distillation</a> ·
<a rel="next" href="#wiki-qlora-and-quantized-adapters">QLoRA and quantized adapters →</a> ·
<a rel="related" href="#wiki-kl-regularized-rlhf">alignment hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For a frozen matrix $W\in\mathbb{R}^{d_{\mathrm{out}}\times d_{\mathrm{in}}}$,

$$
W'
=
W+\frac{\alpha}{r}BA,
$$

$$
B\in\mathbb{R}^{d_{\mathrm{out}}\times r},
\qquad
A\in\mathbb{R}^{r\times d_{\mathrm{in}}},
\qquad
r\ll\min(d_{\mathrm{in}},d_{\mathrm{out}}).
$$

The trainable parameter count is
$r(d_{\mathrm{in}}+d_{\mathrm{out}})$. The original LoRA convention initializes
$A$ randomly and sets $B=0$, so $BA=0$ and training begins at the base model;
some implementations swap the equivalent factor roles.
[QLoRA](#wiki-qlora-and-quantized-adapters) additionally quantizes the frozen
base weights.

**Intuition:** constrain each weight update to a trainable low-rank subspace.

<a id="wiki-qlora-and-quantized-adapters"></a>

### QLoRA and quantized adapters

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-3">↑ section</a> ·
<a rel="prev" href="#wiki-lora">← LoRA</a> ·
<a rel="next" href="#wiki-chain-of-thought-as-latent-variable-marginalization">Chain of thought as latent-variable marginalization →</a> ·
<a rel="related" href="#wiki-kl-regularized-rlhf">alignment hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

QLoRA stores the frozen base matrix in quantized form while training LoRA
parameters in a higher-precision compute type. Abstractly, its blockwise
codebook quantization represents

$$
W_i\approx \hat W_i=s\,c_{q_i},
$$

where $q_i$ indexes a NormalFloat codebook value $c_{q_i}$ and the block scale
$s$ is fixed during adapter training. The forward pass uses

$$
y=x\hat W^\top+\frac{\alpha}{r}xA^\top B^\top,
$$

and gradients flow through the computation to $A$ and $B$, not into the
quantized base codes. QLoRA combines 4-bit NormalFloat storage, double
quantization of quantization constants, and paged optimizer state. Its memory
savings do not mean 4-bit arithmetic everywhere: dequantization and matrix
multiplication commonly use bf16/fp16 accumulation, and activations still
consume substantial memory.

**Intuition:** compress the frozen knowledge, keep the small trainable update
precise, and dequantize only as needed for computation.

<a id="wiki-section-4"></a>

## 4. Reasoning, test-time compute, and recurrent depth

<a id="wiki-chain-of-thought-as-latent-variable-marginalization"></a>

### Chain of thought as latent-variable marginalization

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-4">↑ section</a> ·
<a rel="prev" href="#wiki-qlora-and-quantized-adapters">← QLoRA and quantized adapters</a> ·
<a rel="next" href="#wiki-test-time-compute-scaling-and-passk">Test-time compute scaling and pass@k →</a> ·
<a rel="related" href="#wiki-concept-graph">concept graph</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Let $z$ be a reasoning trace:

$$
\pi(y\mid x)
=
\sum_z\pi(z\mid x)\pi(y\mid x,z).
$$

Ordinary chain-of-thought decoding samples or searches for one $z$.
[Self-consistency](#wiki-majority-vote-and-correlated-samples) samples $K$
traces and estimates the marginal mode by

$$
\hat y
=
\arg\max_y
\sum_{k=1}^{K}\mathbf{1}\{y^{(k)}=y\}.
$$

This voting estimator discards probability weights and works only when answers
can be canonicalized reliably.

**Intuition:** different reasoning paths can support the same answer, so vote
over paths rather than trusting one.

<a id="wiki-test-time-compute-scaling-and-passk"></a>

### Test-time compute scaling and pass@k

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-4">↑ section</a> ·
<a rel="prev" href="#wiki-chain-of-thought-as-latent-variable-marginalization">← Chain of thought as latent-variable marginalization</a> ·
<a rel="next" href="#wiki-standard-depth-versus-recurrent-depth">Standard depth versus recurrent depth →</a> ·
<a rel="related" href="#wiki-chain-of-thought-as-latent-variable-marginalization">reasoning hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Over a finite empirical regime, accuracy sometimes fits a saturating curve such
as

$$
\operatorname{acc}(C)
\approx
a-bC^{-\gamma}.
$$

This is a descriptive fit, not a universal law; task difficulty, search
quality, verifier quality, and the model determine the ceiling. If $n$ samples
contain $c$ correct solutions, the unbiased estimator of pass@$k$ is

$$
\widehat{\operatorname{pass@}k}
=
1-\frac{\binom{n-c}{k}}{\binom{n}{k}},
\qquad n\ge k.
$$

**Intuition:** extra inference compute helps only when it creates useful
diversity or deeper computation that selection can exploit.

<a id="wiki-standard-depth-versus-recurrent-depth"></a>

### Standard depth versus recurrent depth

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-4">↑ section</a> ·
<a rel="prev" href="#wiki-test-time-compute-scaling-and-passk">← Test-time compute scaling and pass@k</a> ·
<a rel="next" href="#wiki-fixed-points-and-deep-equilibrium-models">Fixed points and deep equilibrium models →</a> ·
<a rel="related" href="#wiki-chain-of-thought-as-latent-variable-marginalization">reasoning hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

A standard $L$-layer network has distinct maps:

$$
h^{(\ell+1)}
=
f_\ell(h^{(\ell)}),
\qquad
\ell=0,\ldots,L-1.
$$

A recurrent-depth model can use a prelude $P$, weight-tied core $R$, coda $C$,
and $r$ core iterations:

$$
e=P(x),
\qquad
s_0\sim\mathcal{N}(0,\sigma^2I),
$$

$$
s_{i+1}=R(s_i,e),
\qquad i=0,\ldots,r-1,
$$

$$
y=C(s_r).
$$

The input embedding $e$ is injected at every recurrence, so the learned map is
conditioned on the input rather than free-running. Its limiting behavior leads
to the [fixed-point view](#wiki-fixed-points-and-deep-equilibrium-models). If
the three components contain $l_P,l_R,l_C$ transformer layers, the effective
depth is

$$
l_P+rl_R+l_C,
$$

while the parameterized depth is only

$$
l_P+l_R+l_C.
$$

**Intuition:** spend more serial compute with the same parameters by repeatedly
refining a continuous state.

<a id="wiki-fixed-points-and-deep-equilibrium-models"></a>

### Fixed points and deep equilibrium models

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-4">↑ section</a> ·
<a rel="prev" href="#wiki-standard-depth-versus-recurrent-depth">← Standard depth versus recurrent depth</a> ·
<a rel="next" href="#wiki-contraction-rates-and-stopping-error">Contraction rates and stopping error →</a> ·
<a rel="related" href="#wiki-chain-of-thought-as-latent-variable-marginalization">reasoning hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

If $R(\cdot,e)$ is a contraction in $s$, repeated application converges to

$$
s^*=R(s^*,e).
$$

Differentiating the fixed-point equation gives

$$
\frac{\partial s^*}{\partial\theta}
=
\left(I-J_R(s^*)\right)^{-1}
\left.\frac{\partial R}{\partial\theta}\right|_{s^*},
\qquad
J_R=\frac{\partial R}{\partial s}.
$$

The backward pass solves a linear system involving the Jacobian rather than
storing every forward iteration. Finite recurrent-depth models instead unroll
a chosen number of steps and differentiate through some or all of them; their
stability is characterized more directly by
[contraction rates](#wiki-contraction-rates-and-stopping-error).

**Intuition:** a DEQ defines the answer as an equilibrium; a looped transformer
approximates iterative refinement at a finite depth.

<a id="wiki-contraction-rates-and-stopping-error"></a>

### Contraction rates and stopping error

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-4">↑ section</a> ·
<a rel="prev" href="#wiki-fixed-points-and-deep-equilibrium-models">← Fixed points and deep equilibrium models</a> ·
<a rel="next" href="#wiki-training-a-recurrent-depth-model">Training a recurrent-depth model →</a> ·
<a rel="related" href="#wiki-chain-of-thought-as-latent-variable-marginalization">reasoning hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

If, for fixed input $e$,

$$
\|R(s,e)-R(t,e)\|\le q\|s-t\|,
\qquad 0\le q<1,
$$

then Banach's fixed-point theorem gives a unique fixed point and geometric
convergence:

$$
\|s_r-s^*\|\le q^r\|s_0-s^*\|.
$$

A computable a posteriori bound follows from consecutive iterates:

$$
\|s_r-s^*\|
\le\frac{q}{1-q}\|s_r-s_{r-1}\|.
$$

Neural recurrent blocks are not generally global contractions. A local
Jacobian spectral radius below one suggests local stability, but non-normal
Jacobians can show large transient growth even when every eigenvalue lies
inside the unit disk. Conversely, forcing very strong contraction can erase
useful state before the answer is formed.

**Intuition:** convergence speed is controlled by how strongly one recurrence
shrinks state differences, not simply by the number of loops.

<a id="wiki-training-a-recurrent-depth-model"></a>

### Training a recurrent-depth model

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-4">↑ section</a> ·
<a rel="prev" href="#wiki-contraction-rates-and-stopping-error">← Contraction rates and stopping error</a> ·
<a rel="next" href="#wiki-truncated-backpropagation-bias">Truncated backpropagation bias →</a> ·
<a rel="related" href="#wiki-chain-of-thought-as-latent-variable-marginalization">reasoning hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

The Huginn recurrent-depth recipe samples recurrence counts from a
lognormal-Poisson mixture. With target mean parameter $\bar r$ and
$\sigma=1/2$,

$$
\tau\sim
\mathcal{N}
\left(
\log\bar r-\frac{1}{2}\sigma^2,\,
\sigma^2
\right),
\qquad
r\sim\operatorname{Poisson}(e^\tau)+1.
$$

The training objective averages ordinary next-token loss over examples and
sampled depths:

$$
\mathcal{L}(\theta)
=
\mathbb{E}_{x}
\mathbb{E}_{r\sim\Lambda}
\left[
\mathcal{L}_{\mathrm{NTP}}(m_\theta(x,r),x')
\right].
$$

Truncated backpropagation runs all $r$ forward iterations but detaches the state
before the final $k$:

$$
\frac{\partial\mathcal{L}}{\partial\theta}
\approx
\sum_{i=r-k}^{r-1}
\frac{\partial\mathcal{L}}{\partial s_{i+1}}
\frac{\partial R(s_i,e)}{\partial\theta}.
$$

Activation memory for the recurrent core becomes $O(k)$ rather than $O(r)$.
Huginn used $k=8$ and trained with mean recurrence around 32. Random initial
state and variable depth encourage—but do not prove—path-independent, stable
iteration; [truncation bias](#wiki-truncated-backpropagation-bias) remains.

**Intuition:** expose the shared block to many depths while bounding backward
memory.

<a id="wiki-truncated-backpropagation-bias"></a>

### Truncated backpropagation bias

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-4">↑ section</a> ·
<a rel="prev" href="#wiki-training-a-recurrent-depth-model">← Training a recurrent-depth model</a> ·
<a rel="next" href="#wiki-compute-matched-comparison">Compute-matched comparison →</a> ·
<a rel="related" href="#wiki-chain-of-thought-as-latent-variable-marginalization">reasoning hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For a fully unrolled recurrence, the influence of an early state on a later one
contains a Jacobian product:

$$
\frac{\partial s_r}{\partial s_i}
=\prod_{j=i}^{r-1}J_R(s_j).
$$

Singular values below one make long-range gradients vanish; values above one
can make them explode. Truncating to the last $k$ steps discards parameter
effects mediated only through $s_{r-k}$, so the estimator is generally biased.
It is accurate when the omitted influence has decayed, the detached state is
already near a stable attractor, or the remaining window captures the useful
credit-assignment horizon. Randomizing $k$ or adding losses at intermediate
depths can broaden the supervised horizon, at additional compute or objective
complexity.

**Intuition:** truncated backprop saves activation memory by declaring the
distant past constant, and that declaration is an approximation.

<a id="wiki-compute-matched-comparison"></a>

### Compute-matched comparison

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-4">↑ section</a> ·
<a rel="prev" href="#wiki-truncated-backpropagation-bias">← Truncated backpropagation bias</a> ·
<a rel="next" href="#wiki-visible-traces-versus-latent-iteration">Visible traces versus latent iteration →</a> ·
<a rel="related" href="#wiki-chain-of-thought-as-latent-variable-marginalization">reasoning hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Ignoring attention and embedding details, recurrent compute per generated token
is approximately

$$
C_{\mathrm{recurrent}}
\approx
2(N_P+rN_R+N_C),
$$

compared with $2N_{\mathrm{dense}}$ for a dense model. Equality of total stored
parameters does not determine compute equivalence; the split among prelude,
core, and coda matters. Huginn's 3.5B model had about 1.5B parameters in the
recurrent core and demonstrated useful scaling up to a compute load described
by its authors as equivalent to a 50B dense model.

Naively, each recurrent attention layer needs its own contextual K/V state, so
cache grows with effective recurrent depth. Cache sharing can reduce this, but
it is an architectural or inference approximation whose quality must be
measured.

**Intuition:** recurrent models trade parameter memory for repeated use of a
smaller weight set.

<a id="wiki-visible-traces-versus-latent-iteration"></a>

### Visible traces versus latent iteration

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-4">↑ section</a> ·
<a rel="prev" href="#wiki-compute-matched-comparison">← Compute-matched comparison</a> ·
<a rel="next" href="#wiki-adaptive-computation-time">Adaptive computation time →</a> ·
<a rel="related" href="#wiki-chain-of-thought-as-latent-variable-marginalization">reasoning hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

| Property | Token chain of thought | Recurrent or continuous state |
|---|---|---|
| Intermediate representation | Discrete vocabulary tokens | Dense hidden vectors |
| Extra compute | Extra autoregressive decode steps | Extra internal iterations |
| External inspectability | Usually visible and editable | Usually opaque |
| Branching | Sampling/search across token traces | Superposed or separately searched states |
| Training signal | Token targets or outcome reward | End loss, auxiliary losses, or implicit gradients |
| Main bottleneck | Decode latency and context growth | Serial depth, activation stability, and cache design |

Neither representation guarantees faithful reasoning. A readable trace can be
a post-hoc explanation, while an opaque state can still implement a reliable
algorithm. The distinction is operational: token traces expose an interface for
verification and intervention; latent steps offer a higher-bandwidth internal
workspace.

**Intuition:** visible and latent reasoning spend compute through different
interfaces, trading monitorability for representational bandwidth.

<a id="wiki-adaptive-computation-time"></a>

### Adaptive computation time

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-4">↑ section</a> ·
<a rel="prev" href="#wiki-visible-traces-versus-latent-iteration">← Visible traces versus latent iteration</a> ·
<a rel="next" href="#wiki-coconut">Coconut →</a> ·
<a rel="related" href="#wiki-chain-of-thought-as-latent-variable-marginalization">reasoning hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

A halting unit emits

$$
p_i=\sigma(w^\top s_i+b).
$$

Stop at

$$
N(x)
=
\min\left\{
n:\sum_{i=1}^{n}p_i\ge 1-\epsilon
\right\},
$$

with remainder

$$
\rho=1-\sum_{i=1}^{N-1}p_i.
$$

The output is a weighted state

$$
\bar s
=
\sum_{i=1}^{N}w_is_i,
\qquad
w_i=
\begin{cases}
p_i,&i<N,\\
\rho,&i=N.
\end{cases}
$$

The ACT ponder cost is commonly proportional to $N+\rho$. PonderNet instead
regularizes a learned halting distribution toward a geometric prior. A
zero-shot alternative uses a
[convergence rule](#wiki-contraction-rates-and-stopping-error) such as
$\|s_{i+1}-s_i\|<\delta$, but small state change does not by itself guarantee
answer correctness.

**Intuition:** let each input buy a different number of refinement steps, and
charge the model for taking more.

<a id="wiki-coconut"></a>

### Coconut

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-4">↑ section</a> ·
<a rel="prev" href="#wiki-adaptive-computation-time">← Adaptive computation time</a> ·
<a rel="next" href="#wiki-pause-tokens">Pause tokens →</a> ·
<a rel="related" href="#wiki-chain-of-thought-as-latent-variable-marginalization">reasoning hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

In latent mode, Coconut skips token sampling and embedding lookup:

$$
h_{t+1}^{\mathrm{in}}=h_t^{\mathrm{out}}.
$$

After a configured number of continuous thoughts, ordinary token decoding
resumes. Training uses a curriculum: at stage $k$, remove the first $k$
language reasoning steps and replace each with $c$ continuous thoughts.
The paper's probes support a breadth-first-search-like interpretation on its
synthetic planning tasks, but this is an empirical interpretation—not a
guarantee that a hidden vector literally executes BFS.

**Intuition:** keep intermediate computation in the model's continuous state
instead of forcing every step through a discrete token bottleneck.

<a id="wiki-pause-tokens"></a>

### Pause tokens

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-4">↑ section</a> ·
<a rel="prev" href="#wiki-coconut">← Coconut</a> ·
<a rel="next" href="#wiki-chain-of-thought-internalization">Chain-of-thought internalization →</a> ·
<a rel="related" href="#wiki-chain-of-thought-as-latent-variable-marginalization">reasoning hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Append $m$ learned pause tokens before decoding the supervised answer:

$$
x'=[x,\underbrace{\langle\mathrm{pause}\rangle,\ldots,
\langle\mathrm{pause}\rangle}_{m}],
$$

and omit the pause positions from the output loss. Each pause position creates
another sequence state through every layer before the answer begins. In the
original experiments, the strongest gains required pause tokens during both
pretraining and downstream fine-tuning; adding them only at inference is not
equivalent.

**Intuition:** add scratch positions that carry computation without committing
to semantic output tokens.

<a id="wiki-chain-of-thought-internalization"></a>

### Chain-of-thought internalization

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-4">↑ section</a> ·
<a rel="prev" href="#wiki-pause-tokens">← Pause tokens</a> ·
<a rel="next" href="#wiki-expressivity-what-depth-and-loops-actually-buy">Expressivity: what depth and loops actually buy →</a> ·
<a rel="related" href="#wiki-chain-of-thought-as-latent-variable-marginalization">reasoning hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Stepwise internalization starts with explicit steps
$z=(z_1,\ldots,z_m)$ and gradually removes a prefix during fine-tuning:

$$
\mathcal{L}_k
=
-\log\pi_\theta(z_{k+1:m},y\mid x),
\qquad k=0,\ldots,m.
$$

At $k=m$, the model is trained to emit the answer directly. This removes the
observable trace from the output channel, but it does not prove that a
particular hidden computation was preserved, nor that mutual information
between every possible trace and the answer becomes zero.

**Intuition:** use explicit reasoning as a temporary curriculum, then shorten
the visible scaffold.

<a id="wiki-expressivity-what-depth-and-loops-actually-buy"></a>

### Expressivity: what depth and loops actually buy

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-4">↑ section</a> ·
<a rel="prev" href="#wiki-chain-of-thought-internalization">← Chain-of-thought internalization</a> ·
<a rel="next" href="#wiki-mdps-returns-and-bellman-equations">MDPs, returns, and Bellman equations →</a> ·
<a rel="related" href="#wiki-chain-of-thought-as-latent-variable-marginalization">reasoning hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Complexity claims depend sharply on the attention model, precision,
normalization, width, uniformity, and number of generated steps. Representative
results place certain constant-depth, finite-precision transformers in
$\mathsf{TC}^0$, with tighter $\mathsf{AC}^0$ bounds under constant-bit
assumptions. The distinction matters: parity is not in $\mathsf{AC}^0$, but it
is in $\mathsf{TC}^0$.

With $T$ chain-of-thought steps, constant-depth decoders can simulate broader
classes of serial computations under the assumptions of the relevant
theorems. Looping similarly adds serial depth in continuous state, but it is
not automatically equivalent to $T$ visible CoT tokens: token feedback,
precision, state size, and read/write access differ.

**Intuition:** serial intermediate computation expands expressivity, but the
complexity class follows from the exact model assumptions—not from the word
“transformer.”

<a id="wiki-section-5"></a>

## 5. Self-play, search, and reinforcement learning

<a id="wiki-mdps-returns-and-bellman-equations"></a>

### MDPs, returns, and Bellman equations

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-5">↑ section</a> ·
<a rel="prev" href="#wiki-expressivity-what-depth-and-loops-actually-buy">← Expressivity: what depth and loops actually buy</a> ·
<a rel="next" href="#wiki-entropy-regularization">Entropy regularization →</a> ·
<a rel="related" href="#wiki-expert-iteration-as-an-operator">self-play hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

A Markov decision process is
$(\mathcal{S},\mathcal{A},P,r,\gamma)$. The discounted return is

$$
G_t=\sum_{k=0}^{\infty}\gamma^kr_{t+k}.
$$

The value, action-value, and advantage functions are

$$
V^\pi(s)
=
\mathbb{E}_\pi[G_t\mid s_t=s],
$$

$$
Q^\pi(s,a)
=
\mathbb{E}_\pi[G_t\mid s_t=s,a_t=a],
\qquad
A^\pi(s,a)=Q^\pi(s,a)-V^\pi(s).
$$

The Bellman expectation equation is

$$
V^\pi(s)
=
\mathbb{E}_{a\sim\pi(\cdot\mid s)}
\left[
r(s,a)
+\gamma
\mathbb{E}_{s'\sim P(\cdot\mid s,a)}
V^\pi(s')
\right].
$$

**Intuition:** value is future reward summarized recursively; advantage says
whether one action beats the policy's local average.

<a id="wiki-entropy-regularization"></a>

### Entropy regularization

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-5">↑ section</a> ·
<a rel="prev" href="#wiki-mdps-returns-and-bellman-equations">← MDPs, returns, and Bellman equations</a> ·
<a rel="next" href="#wiki-mcts-with-puct">MCTS with PUCT →</a> ·
<a rel="related" href="#wiki-expert-iteration-as-an-operator">self-play hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

$$
J(\theta)
=
\mathbb{E}
\left[
\sum_t
\left(
r_t+\alpha H(\pi_\theta(\cdot\mid s_t))
\right)
\right],
$$

$$
H(\pi(\cdot\mid s))
=
-\sum_a\pi(a\mid s)\log\pi(a\mid s).
$$

Entropy collapse means $H\to0$. Low entropy is not inherently bad at
convergence, but premature collapse kills exploration and group diversity in
online [GRPO](#wiki-grpo) and other RL.

**Intuition:** pay the policy to keep alternatives alive while it is still
learning.

<a id="wiki-mcts-with-puct"></a>

### MCTS with PUCT

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-5">↑ section</a> ·
<a rel="prev" href="#wiki-entropy-regularization">← Entropy regularization</a> ·
<a rel="next" href="#wiki-mcts-backup-and-value-perspective">MCTS backup and value perspective →</a> ·
<a rel="related" href="#wiki-expert-iteration-as-an-operator">self-play hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

At node $s$, choose

$$
a^*
=
\arg\max_a
\left[
Q(s,a)
+c_{\mathrm{puct}}P(s,a)
\frac{\sqrt{\sum_bN(s,b)}}{1+N(s,a)}
\right].
$$

$P(s,a)$ is the policy-network prior, $N(s,a)$ the edge visit count, and
$Q(s,a)$ the mean [backed-up value](#wiki-mcts-backup-and-value-perspective).
A leaf is expanded with

$$
(p,v)=f_\theta(s),
$$

then $v$ is backed up along the selected path. Visit counts define the improved
policy

$$
\pi_{\mathrm{MCTS}}(a\mid s)
\propto
N(s,a)^{1/\tau}.
$$

**Intuition:** search where either value already looks good or the prior says an
underexplored move deserves a trial.

<a id="wiki-mcts-backup-and-value-perspective"></a>

### MCTS backup and value perspective

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-5">↑ section</a> ·
<a rel="prev" href="#wiki-mcts-with-puct">← MCTS with PUCT</a> ·
<a rel="next" href="#wiki-alphazero-training">AlphaZero training →</a> ·
<a rel="related" href="#wiki-expert-iteration-as-an-operator">self-play hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

After a simulation returns leaf value $v$, each traversed edge updates

$$
N(s,a)\leftarrow N(s,a)+1,
\qquad
W(s,a)\leftarrow W(s,a)+v,
\qquad
Q(s,a)=\frac{W(s,a)}{N(s,a)}.
$$

In alternating zero-sum games, the value is usually expressed from the player
to move, so its sign flips at each ply during backup. Terminal outcomes replace
the network estimate when known. Parallel search may add a temporary virtual
loss or visit count to selected edges, discouraging workers from duplicating
the same simulation; that changes scheduling, not the final value definition.
A biased leaf evaluator can be partly corrected by deeper search only if the
tree reaches evidence that contradicts it.

**Intuition:** selection decides where to gather evidence; backup turns each
simulation into updated empirical action values.

<a id="wiki-alphazero-training"></a>

### AlphaZero training

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-5">↑ section</a> ·
<a rel="prev" href="#wiki-mcts-backup-and-value-perspective">← MCTS backup and value perspective</a> ·
<a rel="next" href="#wiki-expert-iteration-as-an-operator">Expert iteration as an operator →</a> ·
<a rel="related" href="#wiki-expert-iteration-as-an-operator">self-play hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Given self-play outcome $z\in\{-1,0,1\}$, network outputs $(p,v)$, and an MCTS
visit policy $\pi_{\mathrm{MCTS}}$, the loss is

$$
\mathcal{L}
=
(z-v)^2
-\pi_{\mathrm{MCTS}}^\top\log p
+c\|\theta\|_2^2.
$$

**Intuition:** MCTS is the policy-improvement operator; the neural network
distills the resulting policy and terminal value so the next search starts
stronger.

<a id="wiki-expert-iteration-as-an-operator"></a>

### Expert iteration as an operator

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-5">↑ section</a> ·
<a rel="prev" href="#wiki-alphazero-training">← AlphaZero training</a> ·
<a rel="next" href="#wiki-policy-improvement-theorem">Policy improvement theorem →</a> ·
<a rel="related" href="#wiki-concept-graph">concept graph</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Let $\mathcal{I}$ be an improvement operator—search, best-of-$N$, verifier
filtering, or voting. A projection step is

$$
\pi_{k+1}
=
\arg\min_\pi
\mathbb{E}_x
D_{\mathrm{KL}}
\left(
\mathcal{I}[\pi_k](\cdot\mid x)
\|
\pi(\cdot\mid x)
\right).
$$

This improves the true objective only if $\mathcal{I}$ supplies genuinely
better targets and the projection preserves the
[policy improvement](#wiki-policy-improvement-theorem). Optimizing a
misspecified proxy can amplify its errors instead.

**Intuition:** alternate between constructing a better policy and compressing
it back into the model.

<a id="wiki-policy-improvement-theorem"></a>

### Policy improvement theorem

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-5">↑ section</a> ·
<a rel="prev" href="#wiki-expert-iteration-as-an-operator">← Expert iteration as an operator</a> ·
<a rel="next" href="#wiki-two-player-zero-sum-self-play">Two-player zero-sum self-play →</a> ·
<a rel="related" href="#wiki-expert-iteration-as-an-operator">self-play hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For a discounted MDP, suppose a candidate policy $\pi'$ satisfies

$$
\sum_a\pi'(a\mid s)Q^\pi(s,a)\ge V^\pi(s)
\qquad\text{for every }s.
$$

Then

$$
V^{\pi'}(s)\ge V^\pi(s)
\qquad\text{for every }s.
$$

A greedy policy with respect to exact $Q^\pi$ satisfies the premise. Search and
learned critics supply only approximate values, function approximation couples
states, and distillation may not reproduce the improved action distribution;
each approximation can break monotonic improvement.

**Intuition:** choosing actions that are locally no worse under the old value
function yields a globally no-worse policy—when those values and choices are
exact.

<a id="wiki-two-player-zero-sum-self-play"></a>

### Two-player zero-sum self-play

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-5">↑ section</a> ·
<a rel="prev" href="#wiki-policy-improvement-theorem">← Policy improvement theorem</a> ·
<a rel="next" href="#wiki-non-transitivity-and-populations">Non-transitivity and populations →</a> ·
<a rel="related" href="#wiki-expert-iteration-as-an-operator">self-play hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For payoff $u(\pi_1,\pi_2)$ to player 1, a Nash equilibrium satisfies

$$
u(\pi_1^*,\pi_2^*)
=
\max_{\pi_1}\min_{\pi_2}u(\pi_1,\pi_2)
=
\min_{\pi_2}\max_{\pi_1}u(\pi_1,\pi_2).
$$

One-sided exploitability against a candidate opponent $\pi$ is

$$
\epsilon_1(\pi)
=
\max_{\pi'}u(\pi',\pi)-v^*,
$$

where $v^*$ is the game value. A symmetric profile is commonly assessed with a
sum or maximum of both players' best-response gaps. Fictitious play responds to
the empirical mixture

$$
\bar\pi_t=\frac{1}{t}\sum_{i=1}^{t}\pi_i.
$$

In finite two-player zero-sum games, the time-averaged empirical strategies
converge to the equilibrium set even when last-iterate policies cycle.

**Intuition:** strength is defined against best responses, not against the
latest copy of yourself.

<a id="wiki-non-transitivity-and-populations"></a>

### Non-transitivity and populations

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-5">↑ section</a> ·
<a rel="prev" href="#wiki-two-player-zero-sum-self-play">← Two-player zero-sum self-play</a> ·
<a rel="next" href="#wiki-regret-mixtures-and-equilibrium">Regret, mixtures, and equilibrium →</a> ·
<a rel="related" href="#wiki-expert-iteration-as-an-operator">self-play hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For an antisymmetric pairwise payoff matrix $M$, a transitive rating component
has the form

$$
M^{\mathrm{trans}}_{ij}=s_i-s_j.
$$

As a matrix this has rank at most two, not rank one. The residual can contain
cyclic structure such as rock-paper-scissors. When that cyclic component is
large, a single scalar leaderboard discards strategically important
information.

Population or league training, generalized by
[PSRO](#wiki-psro-and-empirical-games), maintains policies
$\{\pi^{(1)},\ldots,\pi^{(K)}\}$ and samples opponents from

$$
\pi_{\mathrm{opp}}
=
\sum_{j=1}^{K}w_j\pi^{(j)}.
$$

The weights can prioritize hard counters, underplayed opponents, or historical
coverage—not simply high win rate.

**Intuition:** keep a portfolio of strategies when “better than” is cyclic.

<a id="wiki-regret-mixtures-and-equilibrium"></a>

### Regret, mixtures, and equilibrium

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-5">↑ section</a> ·
<a rel="prev" href="#wiki-non-transitivity-and-populations">← Non-transitivity and populations</a> ·
<a rel="next" href="#wiki-psro-and-empirical-games">PSRO and empirical games →</a> ·
<a rel="related" href="#wiki-expert-iteration-as-an-operator">self-play hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For rewards $u_t(a)$ over $T$ rounds, external regret is

$$
R_T
=
\max_a\sum_{t=1}^{T}u_t(a)
-\sum_{t=1}^{T}u_t(a_t).
$$

An algorithm is no-regret if $R_T/T\to0$. When both players in a finite
zero-sum game use no-regret algorithms, their time-averaged strategies approach
a Nash equilibrium in the sense that the saddle-point gap is bounded by the
sum of their average regrets:

$$
\max_{\pi_1}u(\pi_1,\bar\pi_2)
-\min_{\pi_2}u(\bar\pi_1,\pi_2)
\le
\frac{R_T^{(1)}+R_T^{(2)}}{T}.
$$

This is an average-policy statement; the latest policies may still rotate
around the equilibrium.

**Intuition:** equilibrium emerges when neither player could have gained much
by replacing its entire history with one fixed strategy.

<a id="wiki-psro-and-empirical-games"></a>

### PSRO and empirical games

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-5">↑ section</a> ·
<a rel="prev" href="#wiki-regret-mixtures-and-equilibrium">← Regret, mixtures, and equilibrium</a> ·
<a rel="next" href="#wiki-spin">SPIN →</a> ·
<a rel="related" href="#wiki-expert-iteration-as-an-operator">self-play hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Policy-space response oracles maintain populations
$\Pi_1,\Pi_2$ and estimate their empirical payoff matrix

$$
M_{ij}=u(\pi_1^{(i)},\pi_2^{(j)}).
$$

A meta-solver computes distributions $(\sigma_1,\sigma_2)$ over the current
populations. Each player then trains an approximate best response,

$$
\pi_i^{\mathrm{new}}
\approx
\arg\max_{\pi_i}
\mathbb{E}_{\pi_{-i}\sim\sigma_{-i}}
[u_i(\pi_i,\pi_{-i})],
$$

adds it to the population, and repeats. Double oracle uses a Nash solver for
the restricted empirical game; other meta-solvers can favor diversity or
recent opponents. The method separates two errors: whether the oracle finds a
real counterstrategy, and whether the finite payoff table accurately measures
interactions.

**Intuition:** solve the small game among known strategies, then expand it with
the strongest missing response.

<a id="wiki-spin"></a>

### SPIN

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-5">↑ section</a> ·
<a rel="prev" href="#wiki-psro-and-empirical-games">← PSRO and empirical games</a> ·
<a rel="next" href="#wiki-star">STaR →</a> ·
<a rel="related" href="#wiki-expert-iteration-as-an-operator">self-play hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

At round $t$, sample $y'\sim\pi_t(\cdot\mid x)$ and treat the human
demonstration $y$ as preferred. A DPO-like round objective is

$$
\mathcal{L}_{\mathrm{SPIN}}
=
-\mathbb{E}
\log\sigma
\left[
\lambda
\log\frac{\pi_{t+1}(y\mid x)}{\pi_t(y\mid x)}
-\lambda
\log\frac{\pi_{t+1}(y'\mid x)}{\pi_t(y'\mid x)}
\right].
$$

Under the paper's idealized assumptions, the target data distribution is the
unique global optimum.

**Intuition:** use the previous model as both opponent and reference, and train
the next model to distinguish demonstrations from self-generated responses.

<a id="wiki-star"></a>

### STaR

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-5">↑ section</a> ·
<a rel="prev" href="#wiki-spin">← SPIN</a> ·
<a rel="next" href="#wiki-self-rewarding-language-models">Self-rewarding language models →</a> ·
<a rel="related" href="#wiki-expert-iteration-as-an-operator">self-play hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For each labeled pair $(x,y^*)$:

1. sample rationale and answer $(z,y)\sim\pi$;
2. retain the trace if $y=y^*$;
3. for failures, generate again while revealing $y^*$ to elicit a
   rationalization;
4. fine-tune on rationales that lead to the correct answer, then repeat.

The original recipe fine-tunes again from the initial pretrained model each
round, using the enlarged rationale set.

**Intuition:** correctness filters self-generated reasoning, while answer-hinted
rationalization recovers training signal from failures.

<a id="wiki-self-rewarding-language-models"></a>

### Self-rewarding language models

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-5">↑ section</a> ·
<a rel="prev" href="#wiki-star">← STaR</a> ·
<a rel="next" href="#wiki-proposer-solver-self-play">Proposer-solver self-play →</a> ·
<a rel="related" href="#wiki-expert-iteration-as-an-operator">self-play hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

At iteration $t$, the model acts as both generator and judge:

$$
y_i\sim\pi_t(\cdot\mid x),
\qquad
r_i=J_{\pi_t}(x,y_i).
$$

Construct preference pairs such as $(y_{\max},y_{\min})$ from judged samples,
then apply iterative [DPO](#wiki-dpo-and-close-variants) to obtain
$\pi_{t+1}$. Generator and judge capability can improve together, but
correlated self-judgment errors can also reinforce themselves.

**Intuition:** remove the frozen reward-model ceiling by letting the judge
co-evolve—at the cost of a shared blind spot.

<a id="wiki-proposer-solver-self-play"></a>

### Proposer-solver self-play

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-5">↑ section</a> ·
<a rel="prev" href="#wiki-self-rewarding-language-models">← Self-rewarding language models</a> ·
<a rel="next" href="#wiki-debate">Debate →</a> ·
<a rel="related" href="#wiki-expert-iteration-as-an-operator">self-play hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

A proposer emits a task $x\sim\pi_P$; a solver emits
$y\sim\pi_S(\cdot\mid x)$; a
[verifier](#wiki-rl-with-verifiable-rewards) returns $v(x,y)$. The solver
reward is

$$
r_S=v(x,y).
$$

The proposer can target learnable difficulty using empirical solver pass rate
$\bar p(x)$:

$$
r_P(x)
=
1-2|\bar p(x)-1/2|,
$$

or

$$
r_P(x)=\bar p(x)(1-\bar p(x)).
$$

These two rewards have the same maximizer but different scale and shape.
Absolute Zero instantiates the idea with a code executor and deduction,
abduction, and induction task modes.

**Intuition:** reward the curriculum generator for finding tasks at the
solver's learning frontier.

<a id="wiki-debate"></a>

### Debate

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-5">↑ section</a> ·
<a rel="prev" href="#wiki-proposer-solver-self-play">← Proposer-solver self-play</a> ·
<a rel="next" href="#wiki-verifier-based-group-gradient">Verifier-based group gradient →</a> ·
<a rel="related" href="#wiki-expert-iteration-as-an-operator">self-play hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Two policies produce arguments $a_1$ and $a_2$; judge $J$ chooses a winner. A
symmetric zero-sum reward is

$$
r_1=
\begin{cases}
+1,&J(a_1,a_2)=1,\\
-1,&J(a_1,a_2)=2,
\end{cases}
\qquad
r_2=-r_1.
$$

The hoped-for oversight benefit depends on assumptions: the judge must be able
to verify local claims, adversarial arguments must expose decisive evidence,
and the game must not reward persuasion divorced from truth.

**Intuition:** turn hard evaluation into comparison between adversarially
selected arguments.

<a id="wiki-verifier-based-group-gradient"></a>

### Verifier-based group gradient

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-5">↑ section</a> ·
<a rel="prev" href="#wiki-debate">← Debate</a> ·
<a rel="next" href="#wiki-temperature-top-k-top-p-and-min-p">Temperature, top-k, top-p, and min-p →</a> ·
<a rel="related" href="#wiki-expert-iteration-as-an-operator">self-play hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Ignoring clipping and KL terms, a mean-centered group update for prompt $x$ is

$$
\hat g_x
\propto
\sum_{i=1}^{G}(r_i-\bar r)
\nabla_\theta\log\pi_\theta(y_i\mid x).
$$

The update is exactly zero when all $r_i$ match. For binary rewards, the chance
of a mixed group is

$$
1-p^G-(1-p)^G.
$$

Larger groups improve the chance of observing both outcomes, but cost more
rollout compute and can increase off-policy staleness.

**Intuition:** group-relative RL learns only from within-prompt disagreement.

<a id="wiki-section-6"></a>

## 6. Inference and sampling

<a id="wiki-temperature-top-k-top-p-and-min-p"></a>

### Temperature, top-k, top-p, and min-p

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-6">↑ section</a> ·
<a rel="prev" href="#wiki-verifier-based-group-gradient">← Verifier-based group gradient</a> ·
<a rel="next" href="#wiki-beam-search-and-length-penalties">Beam search and length penalties →</a> ·
<a rel="related" href="#wiki-prefill-versus-decode">inference hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For logits $\ell_v$ and temperature $\tau>0$,

$$
p_v
=
\frac{\exp(\ell_v/\tau)}
{\sum_{u}\exp(\ell_u/\tau)}.
$$

- Top-$k$: retain only the $k$ largest probabilities, then renormalize.
- Top-$p$: retain the smallest probability-ranked prefix whose cumulative mass
  is at least $p$, then renormalize.
- Min-$p$: retain tokens satisfying
  $p_v\ge p_{\min}\max_u p_u$, then renormalize.

As $\tau\to0^+$, sampling approaches greedy argmax when the maximum is unique.
Filter order matters when methods are combined. Unlike
[beam search](#wiki-beam-search-and-length-penalties), these operators retain
stochastic generation.

**Intuition:** temperature reshapes the whole distribution; truncation removes
its low-probability tail.

<a id="wiki-beam-search-and-length-penalties"></a>

### Beam search and length penalties

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-6">↑ section</a> ·
<a rel="prev" href="#wiki-temperature-top-k-top-p-and-min-p">← Temperature, top-k, top-p, and min-p</a> ·
<a rel="next" href="#wiki-prefill-versus-decode">Prefill versus decode →</a> ·
<a rel="related" href="#wiki-prefill-versus-decode">inference hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Beam search keeps the $B_{\mathrm{beam}}$ highest-scoring partial sequences at
each decoding step. A common score is

$$
s(y_{1:t})
=
\frac{\sum_{i=1}^{t}\log p(y_i\mid y_{<i},x)}
{\left(\frac{c+t}{c+1}\right)^\alpha},
$$

where $\alpha$ controls length normalization and $c$ smooths the short-prefix
regime. Beam search is not globally exact unless the beam is large enough to
retain every prefix that could lead to the optimum. Increasing beam width can
reduce quality under a mismatched likelihood objective by favoring generic,
short, or repetitive high-probability strings. Sampling targets distributional
diversity; beam search targets a high-scoring mode under its chosen sequence
score.

**Intuition:** a beam postpones greedy commitment, but it still searches the
model's score—not the external notion of answer quality.

<a id="wiki-prefill-versus-decode"></a>

### Prefill versus decode

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-6">↑ section</a> ·
<a rel="prev" href="#wiki-beam-search-and-length-penalties">← Beam search and length penalties</a> ·
<a rel="next" href="#wiki-kv-cache">KV cache →</a> ·
<a rel="related" href="#wiki-concept-graph">concept graph</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For prompt length $T$ and model width $d$, prefill processes all prompt tokens
in parallel. Dense projections and FFNs require roughly $O(TN)$ parameter
work, while conventional attention adds $O(LT^2d)$. Decode processes one new
token at a time using cached keys and values: parameter work is $O(N)$ per
token and attention reads $O(LTd)$ cached state as the context grows.

For $S$ generated tokens, causal attention score work scales approximately as

$$
\underbrace{O(LT^2d)}_{\text{prefill}}
+
\underbrace{O\!\left(Ld\sum_{s=1}^{S}(T+s)\right)}_{\text{decode attention}}.
$$

Prefill usually exposes large matrix multiplications and is compute-friendly;
small-batch decode has skinny matrix-vector-like work and is often
memory-bandwidth limited. Time to first token and time per output token thus
stress different kernels and scheduling policies.

**Intuition:** prefill digests a prompt in parallel; decode repeatedly moves
the model and an ever-growing cache to emit one token.

<a id="wiki-kv-cache"></a>

### KV cache

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-6">↑ section</a> ·
<a rel="prev" href="#wiki-prefill-versus-decode">← Prefill versus decode</a> ·
<a rel="next" href="#wiki-paged-kv-cache-management">Paged KV-cache management →</a> ·
<a rel="related" href="#wiki-prefill-versus-decode">inference hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For $L$ layers, $G$ KV heads, head width $d_k$, and $b$ bytes per element,

$$
\text{KV bytes per token}
=
2LGd_kb.
$$

The factor two stores keys and values. For Llama-3-70B in bf16, using
$L=80$, $G=8$, $d_k=128$, and $b=2$,

$$
2\cdot80\cdot8\cdot128\cdot2
=
327{,}680\ \text{bytes/token}.
$$

At 128 Ki tokens this is 40 GiB, or about 43 GB, per sequence before allocator,
metadata, and batching overhead. This is the pressure targeted by
[GQA](#wiki-multi-head-grouped-query-and-multi-query-attention),
[MLA](#wiki-multi-head-latent-attention),
[quantized caches](#wiki-weight-and-kv-cache-quantization),
[paging](#wiki-paged-kv-cache-management), and recurrent cache sharing.

**Intuition:** long-context decode is often a memory-capacity and
memory-bandwidth problem before it is a raw-FLOP problem.

<a id="wiki-paged-kv-cache-management"></a>

### Paged KV-cache management

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-6">↑ section</a> ·
<a rel="prev" href="#wiki-kv-cache">← KV cache</a> ·
<a rel="next" href="#wiki-speculative-decoding">Speculative decoding →</a> ·
<a rel="related" href="#wiki-prefill-versus-decode">inference hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

If each cache block holds $P$ tokens, a request of length $T_i$ receives

$$
n_i=\left\lceil\frac{T_i}{P}\right\rceil
$$

physical blocks and wastes fewer than $P$ token slots in its final block.
Across $R$ live requests, internal fragmentation is therefore bounded by

$$
\text{wasted slots}<R(P-1),
$$

rather than reserving every request's maximum possible context contiguously.
A logical block table maps sequence positions to noncontiguous physical cache
blocks. Copy-on-write lets beams or shared-prefix requests reference the same
physical blocks until their tokens diverge. Paging does not shrink the bytes
of a populated KV entry; it reduces allocator waste and enables flexible
sharing and batching.

**Intuition:** manage attention memory in fixed-size pages so growing,
shrinking, and branching sequences do not require contiguous reservations.

<a id="wiki-speculative-decoding"></a>

### Speculative decoding

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-6">↑ section</a> ·
<a rel="prev" href="#wiki-paged-kv-cache-management">← Paged KV-cache management</a> ·
<a rel="next" href="#wiki-arithmetic-intensity-and-bandwidth">Arithmetic intensity and bandwidth →</a> ·
<a rel="related" href="#wiki-prefill-versus-decode">inference hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

A draft distribution $q$ proposes $\gamma$ tokens. The target distribution
$p$ scores the proposed block in parallel. At draft position $i$, accept
$\tilde y_i$ with probability

$$
\min\left(
1,\frac{p(\tilde y_i\mid\cdot)}{q(\tilde y_i\mid\cdot)}
\right).
$$

At the first rejection, sample from

$$
\operatorname{Normalize}\left([p-q]_+\right).
$$

If every draft token is accepted, draw one extra token from $p$. The resulting
sequence distribution is exactly the target distribution. Under an
independent, constant mean acceptance approximation $\alpha$, expected emitted
tokens per target verification pass are

$$
\sum_{i=0}^{\gamma}\alpha^i
=
\frac{1-\alpha^{\gamma+1}}{1-\alpha}.
$$

Real acceptance events are neither independent nor identically distributed, so
this is a planning approximation. Actual speedup also depends on the
[bandwidth and arithmetic-intensity regime](#wiki-arithmetic-intensity-and-bandwidth).

**Intuition:** let a cheap model guess a block and use one expensive pass to
verify as much of it as possible without changing the target distribution.

<a id="wiki-arithmetic-intensity-and-bandwidth"></a>

### Arithmetic intensity and bandwidth

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-6">↑ section</a> ·
<a rel="prev" href="#wiki-speculative-decoding">← Speculative decoding</a> ·
<a rel="next" href="#wiki-weight-and-kv-cache-quantization">Weight and KV-cache quantization →</a> ·
<a rel="related" href="#wiki-prefill-versus-decode">inference hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For a dense model with $N$ parameters stored at $b$ bytes each, small-batch
decode reads approximately

$$
Nb\ \text{weight-bytes per generated token},
$$

while performing approximately $2N$ arithmetic FLOPs. The weight-only
operational intensity is therefore about

$$
\frac{2}{b}\ \text{FLOPs/byte}
$$

at batch one, and ideally grows roughly with batch size while weights are
reused. The roofline crossover occurs when operational intensity reaches the
accelerator's machine balance,

$$
\frac{\text{peak FLOPs/s}}{\text{memory bandwidth in bytes/s}}.
$$

For an H100-class accelerator in bf16 tensor-core regimes this ratio can be on
the order of 300 FLOPs/byte, but the corresponding batch threshold is not a
universal constant: quantization, kernels, KV traffic, parallelism, and achieved
rather than peak throughput all matter. A bandwidth-only latency floor is

$$
t_{\min}\gtrsim\frac{Nb}{\mathrm{bandwidth}},
$$

not $2Nb/\mathrm{bandwidth}$. In a recurrent-depth model the core weight traffic
scales like $rN_Rb$, plus prelude, coda, activation, and cache traffic.

**Intuition:** repeated arithmetic can be cheap if weights stay resident;
repeated weight movement is what small-batch decode pays for.

<a id="wiki-weight-and-kv-cache-quantization"></a>

### Weight and KV-cache quantization

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-6">↑ section</a> ·
<a rel="prev" href="#wiki-arithmetic-intensity-and-bandwidth">← Arithmetic intensity and bandwidth</a> ·
<a rel="next" href="#wiki-passk">pass@k →</a> ·
<a rel="related" href="#wiki-prefill-versus-decode">inference hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For a real-valued block $x$, a uniform $b$-bit affine quantizer uses

$$
q=\operatorname{clip}
\left(
\operatorname{round}\!\left(\frac{x}{s}\right)+z,\,
q_{\min},q_{\max}
\right),
\qquad
\hat x=s(q-z).
$$

Smaller groups provide more scales and usually less error, but add metadata and
kernel overhead. The deployment objective is activation-weighted output error,
not merely $\|W-\hat W\|_2$: a small weight error in a heavily activated channel
can matter more than a large error in a dormant one. Weight-only quantization
primarily reduces model bytes and decode bandwidth; activation quantization is
needed to exploit low-precision matrix units in compute-bound prefill. KV-cache
quantization reduces per-token cache traffic but its error is reused at every
later query attending to that token.

**Intuition:** quantization trades numerical resolution for capacity and
bandwidth, and the best scale is determined by how the values are used.

<a id="wiki-section-7"></a>

## 7. Evaluation

<a id="wiki-passk"></a>

### pass@k

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-7">↑ section</a> ·
<a rel="prev" href="#wiki-weight-and-kv-cache-quantization">← Weight and KV-cache quantization</a> ·
<a rel="next" href="#wiki-sampling-uncertainty-and-confidence-intervals">Sampling uncertainty and confidence intervals →</a> ·
<a rel="related" href="#wiki-sampling-uncertainty-and-confidence-intervals">evaluation hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For task $x$, suppose $n$ sampled solutions contain $c_x$ correct ones. The
unbiased estimator is

$$
\widehat{\operatorname{pass@}k}
=
\mathbb{E}_x
\left[
1-\frac{\binom{n-c_x}{k}}{\binom{n}{k}}
\right],
\qquad n\ge k.
$$

For $k=1$, this reduces to mean sample accuracy $c_x/n$, averaged over tasks.
Generating $n>1$ samples can reduce estimator variance, but samples must match
the sampling policy being evaluated and retain
[prompt-level uncertainty](#wiki-sampling-uncertainty-and-confidence-intervals).

**Intuition:** estimate the chance that at least one of $k$ draws succeeds,
without enumerating every subset.

<a id="wiki-sampling-uncertainty-and-confidence-intervals"></a>

### Sampling uncertainty and confidence intervals

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-7">↑ section</a> ·
<a rel="prev" href="#wiki-passk">← pass@k</a> ·
<a rel="next" href="#wiki-majority-vote-and-correlated-samples">Majority vote and correlated samples →</a> ·
<a rel="related" href="#wiki-concept-graph">concept graph</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For $n$ independent Bernoulli items with empirical accuracy
$\hat p=c/n$, the plug-in standard error is

$$
\operatorname{SE}(\hat p)
\approx
\sqrt{\frac{\hat p(1-\hat p)}{n}}.
$$

The normal interval performs poorly near zero or one and at small $n$. A Wilson
interval with normal quantile $z$ is centered at

$$
\frac{\hat p+z^2/(2n)}{1+z^2/n}
$$

with half-width

$$
\frac{z}{1+z^2/n}
\sqrt{
\frac{\hat p(1-\hat p)}{n}
+\frac{z^2}{4n^2}
}.
$$

Benchmark items are rarely identically distributed, and multiple samples from
one prompt are correlated through shared difficulty. For system comparisons,
resample prompts—not individual generations—in a paired bootstrap so each
replicate preserves the matchup and prompt-level clustering.

**Intuition:** a benchmark score is an estimate with a sampling distribution,
not a property known to every displayed decimal place.

<a id="wiki-majority-vote-and-correlated-samples"></a>

### Majority vote and correlated samples

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-7">↑ section</a> ·
<a rel="prev" href="#wiki-sampling-uncertainty-and-confidence-intervals">← Sampling uncertainty and confidence intervals</a> ·
<a rel="next" href="#wiki-elo-and-bradley-terry-arenas">Elo and Bradley-Terry arenas →</a> ·
<a rel="related" href="#wiki-sampling-uncertainty-and-confidence-intervals">evaluation hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

If each of $K$ odd, independent samples is correct with probability $p$, and
all correct samples agree on one answer, majority-vote accuracy is

$$
\Pr(\text{majority correct})
=
\sum_{j=(K+1)/2}^{K}
\binom{K}{j}p^j(1-p)^{K-j}.
$$

This improves on $p$ when $p>1/2$ under the assumptions. Real model samples are
correlated, wrong answers can split across many labels, and correctness may
require semantic canonicalization. A rough exchangeable model with pairwise
indicator correlation $\rho$ has

$$
\operatorname{Var}\!\left(\frac1K\sum_iX_i\right)
=\frac{p(1-p)}{K}\bigl[1+(K-1)\rho\bigr],
$$

so positive correlation limits the effective benefit of more samples.

**Intuition:** self-consistency works through independent error correction;
duplicating the same mistake adds compute but little evidence.

<a id="wiki-elo-and-bradley-terry-arenas"></a>

### Elo and Bradley-Terry arenas

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-7">↑ section</a> ·
<a rel="prev" href="#wiki-majority-vote-and-correlated-samples">← Majority vote and correlated samples</a> ·
<a rel="next" href="#wiki-calibration">Calibration →</a> ·
<a rel="related" href="#wiki-sampling-uncertainty-and-confidence-intervals">evaluation hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

The Elo win-probability parameterization is

$$
\Pr(A\text{ beats }B)
=
\frac{1}{1+10^{(R_B-R_A)/400}}.
$$

A sequential update is

$$
R_A\leftarrow R_A+K(S_A-\mathbb{E}[S_A]).
$$

Model arenas often fit all pairwise comparisons jointly by maximizing the
Bradley-Terry likelihood rather than relying on order-dependent sequential Elo
updates. A scalar rating assumes largely transitive preferences and should be
supplemented when style, judge, or matchup effects are strong.

**Intuition:** ratings compress pairwise win probabilities into one latent
strength axis.

<a id="wiki-calibration"></a>

### Calibration

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-7">↑ section</a> ·
<a rel="prev" href="#wiki-elo-and-bradley-terry-arenas">← Elo and Bradley-Terry arenas</a> ·
<a rel="next" href="#wiki-proper-scoring-rules">Proper scoring rules →</a> ·
<a rel="related" href="#wiki-sampling-uncertainty-and-confidence-intervals">evaluation hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For confidence bins $B_m$, expected calibration error is estimated by

$$
\operatorname{ECE}
=
\sum_{m=1}^{M}
\frac{|B_m|}{n}
\left|
\operatorname{acc}(B_m)-\operatorname{conf}(B_m)
\right|.
$$

ECE is bin-dependent and is not a
[proper scoring rule](#wiki-proper-scoring-rules). Report the binning scheme
and pair it with reliability diagrams or proper scores such as log loss or
Brier score. “Confidence” must also be defined: next-token probability,
sequence probability, verbalized confidence, and answer correctness are
different calibration targets.

**Intuition:** calibrated 70% predictions should be correct about 70% of the
time among comparable predictions.

<a id="wiki-proper-scoring-rules"></a>

### Proper scoring rules

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-7">↑ section</a> ·
<a rel="prev" href="#wiki-calibration">← Calibration</a> ·
<a rel="next" href="#wiki-contamination-and-membership-checks">Contamination and membership checks →</a> ·
<a rel="related" href="#wiki-sampling-uncertainty-and-confidence-intervals">evaluation hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

For binary outcome $y\in\{0,1\}$ and forecast probability $p$, the Brier score
and log loss are

$$
\operatorname{Brier}(p,y)=(p-y)^2,
$$

$$
\operatorname{LogLoss}(p,y)
=-[y\log p+(1-y)\log(1-p)].
$$

Both are proper: in expectation, reporting the true probability minimizes the
score. Log loss penalizes confident mistakes without a finite bound; Brier
score is bounded and decomposes into reliability, resolution, and uncertainty.
ECE can decrease through fortunate binning even when probabilistic forecasts
worsen, so a proper score should carry the primary comparison.

**Intuition:** a proper score rewards honest probabilities, not merely correct
labels or visually convenient calibration bins.

<a id="wiki-contamination-and-membership-checks"></a>

### Contamination and membership checks

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-7">↑ section</a> ·
<a rel="prev" href="#wiki-proper-scoring-rules">← Proper scoring rules</a> ·
<a rel="next" href="#wiki-multiple-comparisons-and-benchmark-saturation">Multiple comparisons and benchmark saturation →</a> ·
<a rel="related" href="#wiki-sampling-uncertainty-and-confidence-intervals">evaluation hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

Direct contamination checks measure exact or approximate $n$-gram overlap
between evaluation items and the training corpus. When the corpus is
unavailable, Min-$K\%$ Prob sorts a sequence's token log-probabilities and
averages the least-likely fraction:

$$
\operatorname{MinK}_k(x)
=
\frac{1}{|\mathcal{I}_k|}
\sum_{t\in\mathcal{I}_k}
\log\pi_\theta(x_t\mid x_{<t}),
$$

where $\mathcal{I}_k$ indexes the lowest-probability $k\%$ of tokens. Seen text
may have unusually high values because it lacks surprising low-probability
tokens. This is a statistical membership signal, not proof that a specific
item was in training; paraphrase, deduplication, domain shift, and model size
affect it. Repeatedly selecting the best contamination detector or benchmark
slice also invokes the
[multiple-comparisons problem](#wiki-multiple-comparisons-and-benchmark-saturation).

**Intuition:** memorized text tends to have fewer token-level surprises, but
only corpus provenance can establish contamination directly.

<a id="wiki-multiple-comparisons-and-benchmark-saturation"></a>

### Multiple comparisons and benchmark saturation

<nav class="math-hypermedia" aria-label="Concept navigation">
<a rel="up" href="#wiki-section-7">↑ section</a> ·
<a rel="prev" href="#wiki-contamination-and-membership-checks">← Contamination and membership checks</a> ·
<a rel="next" href="#wiki-sources">primary references →</a> ·
<a rel="related" href="#wiki-sampling-uncertainty-and-confidence-intervals">evaluation hub</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a> ·
<a rel="describedby" href="#wiki-sources">sources</a>
</nav>

If $m$ independent null hypotheses are each tested at level $\alpha$, the
probability of at least one false rejection is

$$
1-(1-\alpha)^m,
$$

which is approximately $m\alpha$ for small $\alpha$. Bonferroni controls the
family-wise error rate by testing each comparison at $\alpha/m$; false
discovery rate procedures trade stricter family-wise guarantees for power.
Repeatedly choosing prompts, checkpoints, templates, or random seeds after
viewing the same benchmark is also multiple testing, even when no formal
$p$-values are reported.

Near saturation, a benchmark has little headroom and its remaining errors may
be mislabeled, contaminated, or unrepresentative. Report per-category results,
paired uncertainty, and evaluation decisions made after observing the test set;
a tiny aggregate gain over a repeatedly tuned public benchmark is weak evidence
of broad capability improvement.

**Intuition:** the more evaluation choices you try, the less surprising the
best-looking score becomes.

<a id="wiki-sources"></a>

## Selected primary references

<nav class="math-hypermedia" aria-label="Reference navigation">
<a rel="up" href="#math-wiki">↑ wiki home</a> ·
<a rel="prev" href="#wiki-multiple-comparisons-and-benchmark-saturation">← final concept</a> ·
<a rel="index" href="#wiki-concept-graph">graph</a> ·
<a rel="help" href="#wiki-symbols">symbols</a>
</nav>

- Vaswani et al., [Attention Is All You Need](https://arxiv.org/abs/1706.03762).
- Dao et al., [FlashAttention: Fast and Memory-Efficient Exact Attention with
  IO-Awareness](https://arxiv.org/abs/2205.14135).
- Shazeer, [GLU Variants Improve
  Transformer](https://arxiv.org/abs/2002.05202).
- Zhang and Sennrich, [Root Mean Square Layer
  Normalization](https://arxiv.org/abs/1910.07467).
- Fedus et al., [Switch Transformers: Scaling to Trillion Parameter Models
  with Simple and Efficient Sparsity](https://arxiv.org/abs/2101.03961).
- Su et al., [RoFormer: Enhanced Transformer with Rotary Position
  Embedding](https://arxiv.org/abs/2104.09864).
- Press et al., [Train Short, Test Long: Attention with Linear Biases Enables
  Input Length Extrapolation](https://arxiv.org/abs/2108.12409).
- Ainslie et al., [GQA: Training Generalized Multi-Query Transformer Models
  from Multi-Head Checkpoints](https://arxiv.org/abs/2305.13245).
- DeepSeek-AI, [DeepSeek-V2: A Strong, Economical, and Efficient
  Mixture-of-Experts Language Model](https://arxiv.org/abs/2405.04434).
- Gu and Dao, [Mamba: Linear-Time Sequence Modeling with Selective State
  Spaces](https://arxiv.org/abs/2312.00752).
- Hoffmann et al., [Training Compute-Optimal Large Language
  Models](https://arxiv.org/abs/2203.15556).
- Rafailov et al., [Direct Preference Optimization: Your Language Model Is
  Secretly a Reward Model](https://arxiv.org/abs/2305.18290).
- Schulman et al., [High-Dimensional Continuous Control Using Generalized
  Advantage Estimation](https://arxiv.org/abs/1506.02438).
- Schulman et al., [Proximal Policy Optimization
  Algorithms](https://arxiv.org/abs/1707.06347).
- Ethayarajh et al., [KTO: Model Alignment as Prospect Theoretic
  Optimization](https://arxiv.org/abs/2402.01306).
- Meng et al., [SimPO: Simple Preference Optimization with a Reference-Free
  Reward](https://arxiv.org/abs/2405.14734).
- Shao et al., [DeepSeekMath: Pushing the Limits of Mathematical Reasoning in
  Open Language Models](https://arxiv.org/abs/2402.03300).
- Yu et al., [DAPO: An Open-Source LLM Reinforcement Learning System at
  Scale](https://arxiv.org/abs/2503.14476).
- Beirami et al., [Theoretical Guarantees on the Best-of-N Alignment
  Policy](https://arxiv.org/abs/2401.01879).
- Gao et al., [Scaling Laws for Reward Model
  Overoptimization](https://arxiv.org/abs/2210.10760).
- Lightman et al., [Let's Verify Step by
  Step](https://arxiv.org/abs/2305.20050).
- Hu et al., [LoRA: Low-Rank Adaptation of Large Language
  Models](https://arxiv.org/abs/2106.09685).
- Dettmers et al., [QLoRA: Efficient Finetuning of Quantized
  LLMs](https://arxiv.org/abs/2305.14314).
- Geiping et al., [Scaling up Test-Time Compute with Latent Reasoning: A
  Recurrent Depth Approach](https://arxiv.org/abs/2502.05171).
- Hao et al., [Training Large Language Models to Reason in a Continuous Latent
  Space](https://arxiv.org/abs/2412.06769).
- Goyal et al., [Think Before You Speak: Training Language Models With Pause
  Tokens](https://arxiv.org/abs/2310.02226).
- Wang et al., [Self-Consistency Improves Chain of Thought Reasoning in
  Language Models](https://arxiv.org/abs/2203.11171).
- Bai et al., [Deep Equilibrium
  Models](https://arxiv.org/abs/1909.01377).
- Graves, [Adaptive Computation Time for Recurrent Neural
  Networks](https://arxiv.org/abs/1603.08983).
- Banino et al., [PonderNet: Learning to
  Ponder](https://arxiv.org/abs/2107.05407).
- Li et al., [Chain of Thought Empowers Transformers to Solve Inherently Serial
  Problems](https://arxiv.org/abs/2402.12875).
- Merrill and Sabharwal, [The Expressive Power of Transformers with Chain of
  Thought](https://arxiv.org/abs/2310.07923).
- Silver et al., [Mastering Chess and Shogi by Self-Play with a General
  Reinforcement Learning Algorithm](https://arxiv.org/abs/1712.01815).
- Lanctot et al., [A Unified Game-Theoretic Approach to Multiagent
  Reinforcement Learning](https://arxiv.org/abs/1711.00832).
- Chen et al., [Self-Play Fine-Tuning Converts Weak Language Models to Strong
  Language Models](https://arxiv.org/abs/2401.01335).
- Zelikman et al., [STaR: Bootstrapping Reasoning With
  Reasoning](https://arxiv.org/abs/2203.14465).
- Yuan et al., [Self-Rewarding Language
  Models](https://arxiv.org/abs/2401.10020).
- Zhao et al., [Absolute Zero: Reinforced Self-play Reasoning with Zero
  Data](https://arxiv.org/abs/2505.03335).
- Leviathan et al., [Fast Inference from Transformers via Speculative
  Decoding](https://arxiv.org/abs/2211.17192).
- Kwon et al., [Efficient Memory Management for Large Language Model Serving
  with PagedAttention](https://arxiv.org/abs/2309.06180).
- Lin et al., [AWQ: Activation-aware Weight Quantization for LLM Compression
  and Acceleration](https://arxiv.org/abs/2306.00978).
- Chen et al., [Evaluating Large Language Models Trained on
  Code](https://arxiv.org/abs/2107.03374).
- Guo et al., [On Calibration of Modern Neural
  Networks](https://arxiv.org/abs/1706.04599).
- Shi et al., [Detecting Pretraining Data from Large Language
  Models](https://arxiv.org/abs/2310.16789).
