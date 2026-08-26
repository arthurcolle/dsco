# Entropix Code Study — xjdr-alt/entropix

**Studied commit:** `0a7f7bb25ee50e5fc54a1b59953a718de1a6e1ef` (2024-11-08)  
**Scope:** model forward pass, weights/tokenizer/KV cache, adaptive sampler math, engine/orchestrator/server, eval harness, UI, defects, and Matrix/Qwen portability.

## Executive finding

This checkout contains two partially merged sampler generations:

1. `dslider.py` + `dslider_config.py`: the substantive adaptive Dirichlet sampler. It maintains history-dependent entropy/cross-entropy/temperature/Dirichlet state and chooses among argmax, temperature-tuned top-k, interpolated Dirichlet, and OOD Dirichlet policies.
2. `sampler.py`: a quadrant wrapper (LELV/HELV/LEHV/HEHV) around the adaptive sampler. In this commit its integration is internally inconsistent and several branches are placeholders.

The repository is not runnable end-to-end at the studied commit without repair. The useful intellectual core is `adaptive_dirichlet_step`; the server and UI are unfinished scaffolding. A faithful Qwen port requires decoder-level full logits and persistent per-sequence sampler state. Matrix LM Studio's returned top-20 logprobs are useful telemetry but cannot implement this sampler after the fact.

## 1. Model and token path

### Architecture

`entropix/model.py` implements a compact Llama decoder:

- token embedding lookup (`model.py:63-64`);
- per-layer RMSNorm, grouped-query attention, residual, SwiGLU feed-forward (`model.py:65-69`);
- final RMSNorm and unembedding (`model.py:70`);
- RoPE applied by complex multiplication to Q/K (`model.py:23-32`);
- K/V cache written by layer and position (`kvcache.py:20-26`).

The attention function returns **pre-softmax attention scores from only the last layer** (`model.py:44-55`, overwritten each loop at `65-70`). The current adaptive sampler does not consume these attention scores; its “scaffold” is a Dirichlet-derived distribution, not attention entropy.

### Configuration

`config.py` defines only Llama 3.2-style 1B and 70B layouts, both with vocabulary 128,256 and max sequence length 4,096 (`config.py:25-52`). `create_model_params` drops embedding/FFN/vocabulary fields because execution infers them from weights (`67-77`).

### KV cache

The cache is dense, preallocated BF16 `[layers,batch,max_seq,kv_heads,head_dim]` (`kvcache.py:13-18`). Every update repeats KV heads to query-head count across the entire cache (`20-24`), which is simple but memory-expensive. Decode-position handling must be repaired: `local_main.py` sets `cur_pos=seqlen`, then increments before the first single-token forward, likely leaving a one-position gap (`84-90`).

### Weights

`weights.py` loads one `.npy` per tensor, transposes projection matrices, reshapes Q/K/V to `[embed,heads,head_dim]`, and shards over a JAX mesh (`69-115`). This is not a GGUF loader.

`download_weights.py` is unsafe as a generic converter:

- it dereferences `t_path` even if no token was found (`81-83`);
- Q and K reverse-permutation dimensions are hard-coded to 70B (`89-104`), despite README instructions for 1B;
- it loads the whole HF model and then emits BF16 NumPy tensors (`83-114`).

### Tokenizer and prompt construction

`tokenizer.py` is a faithful Llama 3 tiktoken wrapper with 256 reserved specials (`34-163`).

`prompts.py` contains a critical role inversion:

```python
role_to_header = {"system":"user", "user":"assistant", "assistant":"user"}
```

Then `generate_chat_prompt` uses that map (`53-64`). User messages therefore become assistant headers, and assistant messages become user headers. It also does not append a final assistant-generation header. This invalidates server/eval behavior until fixed.

## 2. Adaptive Dirichlet sampler

### Metrics

For full-vocabulary log probabilities `logp`, `ent_varent` computes:

- entropy: `H = -Σ p log p`;
- varentropy: `V = Σ p (log p + H)^2`.

See `dslider.py:19-26`.

`normalize_logits` subtracts max and logsumexp, then clamps log probabilities below `noise_floor` to `log(EPS)` **without renormalizing afterward** (`29-35`). Consequently the resulting exponentials need not sum exactly to one, especially when many vocabulary entries are floored. All downstream “probabilities,” entropy, KL, and Dirichlet fits inherit this approximation.

### Persistent state

`DSState` tracks 13 quantities (`dslider.py:38-52`):

- exponentially weighted Dirichlet parameters and support log probabilities;
- temperature;
- “naked” model entropy/varentropy;
- “scaffold” entropy/varentropy;
- token-level cross entropy/variance under both distributions;
- Dirichlet surprise;
- top-k naked entropy.

Initialization uses all prefill positions, fits a Dirichlet to mean support log probabilities, and seeds moving averages (`54-97`). This means a real integration needs **prefill logits across positions**, not merely the final API token distribution.

### Selection pipeline

`adaptive_dirichlet_step` (`dslider.py:100-291`) performs:

1. Normalize current logits and compute naked entropy/varentropy (`112-121`).
2. Build history vectors from token cross-entropy and EMWA statistics (`122-140`).
3. Compute a learned/formulaic outlier score with bilinear and linear terms (`141-144`, `299-308`).
4. For non-outliers:
   - concentrated top-k distribution → argmax (`152-164`);
   - otherwise tune temperature to historical top-k entropy and sample categorically (`166-178`).
5. For outliers:
   - solve for temperature matching a target entropy (`180-191`);
   - update the historical support distribution based on KL (`193-205`);
   - fit new Dirichlet concentration parameters (`205`);
   - compare current distribution likelihood under historical Dirichlet (`209-219`);
   - if in-distribution enough, interpolate sampled Dirichlet probabilities with current probabilities and choose argmax (`220-237`);
   - if OOD, sample directly from the Dirichlet support (`239-243`).
6. Update all moving statistics based on the emitted token (`244-291`).

### Numerical solvers

`dslider_utils.py` contains two nontrivial solvers:

- `fit_dirichlet`: Halley updates concentration parameters to match expected log probabilities, with a decaying oscillatory learning rate, clipping updates to ±half the current alpha (`101-186`).
- `temp_tune`: Halley/Newton/gradient fallback to solve `entropy(logits/T)=target`, clipping temperature changes to ±50% per step (`189-248`).

These are the core reusable algorithms.

### Configuration semantics

The default support is the entire 128,256-token vocabulary (`dslider_config.py:246-280`), despite comments recommending an empirically selected support. That makes a 140-iteration Dirichlet fit over the entire vocabulary extremely expensive at initialization and every decode update.

The default outlier threshold has all-positive coefficients and zero bias (`265-273`). Since entropy and standard-deviation terms are nonnegative, the computed score is generally positive, making `outlier_mask = threshold > 0` likely true almost always. This appears untuned or semantically incomplete.

### Quadrant wrapper defects

`sampler.py` wraps the adaptive step in four entropy/varentropy quadrants (`74-155`), but:

- overlapping conditions are resolved by `argmax`, which picks the first true case, not necessarily the intended priority (`74-102`);
- if all predicates are false, all zeros also produce index 0, incorrectly selecting LELV instead of `default`; the switch has five branches but `case` is formed from four values (`102,155`);
- HELV hard-codes token 2564 and ignores its `clarifying_question_token` argument (`40,118`);
- LEHV tree search is explicitly TODO and returns the original token (`120-127`);
- HEHV excludes `new_token` with indexing whose shapes are questionable (`135`);
- every stochastic operation uses the same default PRNG key 1337 unless callers supply/split keys (`41,142`), encouraging deterministic repeated samples/correlation;
- it calls deprecated `jax.tree_map` (`144,148`);
- it passes per-example `state` through `vmap` even though state shape handling around the HEHV branch is fragile.

## 3. Entrypoints are from incompatible revisions

`local_main.py` imports `SamplerConfig` and calls `sample(logits, scores, cur_pos, cfg=...)` (`13,70,91`), but current `sample` signature is `sample(state, logits, config, clarifying_question_token, key)` (`sampler.py:35-42`). It cannot run.

It also expects `xfmr` to return four values (`local_main.py:81,90`), while current `xfmr` returns three (`model.py:70-71`).

`engine.py` is closer to the current sampler but has major integration defects:

- abstract-looking methods have empty bodies (`193-210`, `239-245`);
- `init_decode_state()` returns `None`, yet the generation thread uses it as a decode dictionary;
- `get_prefix_destination_sharding()` returns `None`, but transfer calls `jax.device_put` with it;
- prefill ignores its sampler and returns top-k tokens rather than a sampled first token (`300-366`);
- it indexes `logits[:, true_length]` despite true length/bucket padding semantics, likely selecting a padded/off-by-one position (`338`);
- the prefix's `tokens` has top-k rows, but cache batch size originates from a one-row prompt, then `insert` broadcasts cache in a way conflating samples and batch slots (`338-365`, `468-485`);
- generate uses dictionary indexing despite `DecodeState` being declared a NamedTuple (`77-85`, `389-440`);
- generated length uses `generated_tokens[:, -1] + 1`, while that field is repeatedly incremented as an array (`417,437`);
- `samples_per_slot` claims 1, while prefill top-k defaults to 6 and ResultTokens records inconsistent batch/sample semantics (`217-220`, `338-358`).

`engine_main.py` is from another incompatible revision:

- imports nonexistent `LLAMA_1B_PARAMS` from `engine` (`8`);
- constructs `EntropixEngine` without required `mesh` (`47-52`);
- passes prompt strings into a request field annotated as JAX tokens (`23-31,74-76`).

## 4. Orchestrator and server

The orchestration concept is a JetStream-derived four-stage pipeline:

```text
request → prefill queue → transfer queue → generation slots
        → detokenization queue → per-request AsyncMultifuture
```

This is documented at `orchestrator.py:117-170` and implemented through dedicated prefill, transfer, generate, and detokenize threads (`302-459`, `540-838`). The architecture is useful: queues decouple host work and avoid making detokenization block accelerator generation.

But the current engine contract cannot satisfy it:

- transfer calls the unimplemented sharding method (`598-610`);
- generation starts with unimplemented `init_decode_state` (`646-658`);
- detokenization checks tuple positions/types inconsistently with what prefill enqueues (`577-583` versus `779-802`);
- buffering logic exists (`should_buffer_response`) but `decode` never invokes it before flushing (`850-918`);
- stop handling can mutate `return_channel=None` without cleanly completing consumers (`460-500`).

The FastAPI server provides `/v1/chat/completions` and `/health` (`server_main.py:267-287`). It initializes one prefill and generation engine per JAX device while sharing the same weights (`97-124`). Problems:

- request `temperature` is accepted but never passed to decoding/sampler (`47-52`, `198-203`, `234-240`);
- model request ID is not validated against loaded model;
- usage is hard-coded to zero (`253-264`);
- no logprobs or entropy telemetry is returned;
- stream finalization depends on `token_batch` existing (`216-224`);
- authentication prints invalid API keys and full headers (`184-188`), a credential leak;
- prompt role inversion makes generated prompts invalid;
- `uvicorn.run("server_main:app")` may fail from package-root invocation because module path is actually `entropix.server_main` (`295-303`).

## 5. Evaluation code

The eval suite is adapted from OpenAI's simple-evals style and covers MMLU, MATH, GPQA, MGSM, DROP, HumanEval, and SimpleQA. It uses a generic `SamplerBase`, parallel map, aggregate metrics, and HTML reports (`evals/common.py`).

`EntropixSampler` is only an OpenAI client pointed at `localhost:8000/v1` (`evals/sampler/entropix_sampler.py:17-68`); it does not itself expose or configure Entropix policies.

The Entropix eval driver currently runs only MMLU and GPQA (`evals/entropix.py:74-78`). More importantly, it creates its `grading_sampler` and `equality_checker` as `EntropixSampler` while naming models `gpt-4o` and `gpt-4-turbo-preview` (`46-48`). Since the local server ignores/does not route model IDs, those are not hosted OpenAI graders; they would hit the same local 1B model if the server worked. This undermines math grading and any claim of independent grading.

No checked-in result artifact demonstrates sampler gains. README claims “initial eval results look incredible,” but this checkout contains no corresponding reproducible score table for Entropix.

## 6. UI

The Next.js UI is a disconnected prototype:

- `ChatArea` simulates a response with `setTimeout`; there is no API fetch or SSE consumption (`ui/components/ChatArea/index.tsx:46-96`);
- model IDs are stale SAX paths (`29-32`);
- sidebars react to custom events that no active API integration emits;
- entropy/varentropy visualization exists only as a TODO (`ui/TODO.md`);
- artifacts/code execution are simulated.

It contributes no usable sampler instrumentation today.

## 7. What is real and reusable

### Strongest components

1. Full-distribution entropy and varentropy definitions.
2. History-aware per-sequence sampler state.
3. Entropy-targeted temperature solver.
4. Dirichlet fit over expected log probabilities.
5. Distinction between concentrated inliers, dispersed inliers, familiar outliers, and OOD distributions.
6. Queue-separated prefill/generate/detokenize architecture as a design pattern.

### Not established

1. That the default coefficients are calibrated.
2. That quadrant actions improve quality.
3. That the checked-in server executes.
4. That the method beats ordinary temperature/top-p on held-out tasks.
5. That post-hoc top-k logprobs can reproduce it.

## 8. Matrix/Qwen port

### Current Matrix path

Matrix serves Qwen GGUF models through LM Studio. DSCO now preserves returned logprobs, but those are emitted **after** the server has selected tokens. This permits observability and offline policy analysis, not token intervention.

### Faithful requirements

A faithful port needs, at every sequence position:

1. full pre-sampling vocabulary logits;
2. persistent state per request/beam;
3. control of the emitted token;
4. deterministic and correctly split RNG keys;
5. tokenizer-specific support selection for Qwen vocabulary;
6. prefill logits or a revised state initializer;
7. calibrated thresholds against Qwen distributions;
8. baseline/eval comparisons and deterministic verifier tasks.

### Best implementation boundary

Do **not** port the repository's Llama model, server, or UI. Port the sampler math into the decoder we control:

- preferred: a `llama.cpp` custom sampler chain/module because Matrix models are GGUF and llama.cpp already owns logits/KV state;
- alternative: MLX model loading and a Python/Swift/C++ decode loop exposing logits;
- weakest: LM Studio API shadow mode only.

For llama.cpp, implement a sampler context containing DSState-like arrays and callbacks equivalent to `apply`, `accept`, `reset`, `clone`, and `free`. The initial Qwen implementation should simplify the research code:

1. compute exact full-logit H/V;
2. maintain EWMAs;
3. choose argmax vs entropy-targeted temperature top-k;
4. record policy decisions;
5. add Dirichlet support only after profiling/calibration;
6. defer quadrant hard-coded clarification/tree-search actions.

This staged port isolates whether entropy-targeted adaptive temperature adds value before importing the most expensive and least calibrated Dirichlet machinery.

## 9. Verification plan

1. Repair a frozen upstream copy enough to run Llama 3.2 1B baseline and adaptive sampler.
2. Unit-test entropy/varentropy, temperature solver, Dirichlet fit, state updates, and branch selection on synthetic logits.
3. Build Qwen/llama.cpp sampler behind a runtime flag.
4. Evaluate paired seeds on arithmetic failure set, MMLU subset, GPQA subset, code tests, and open-ended diversity.
5. Log token, H, V, branch, temperature, selected rank, and verifier outcome.
6. Compare greedy, temperature/top-p, adaptive-temperature, and full Dirichlet variants.
7. Reject any sampler that improves self-likelihood but not held-out correctness or diversity-quality metrics.

## Conclusion

Entropix is a valuable research sketch with a serious mathematical core, but this commit is a broken integration snapshot, not a deployable sampler server. The code supports building a real custom sampler for Matrix only if we move token selection into a decoder-level interface. The correct transplant is the adaptive state and policy mathematics—not the repository wholesale.
