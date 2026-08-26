# Fast local target + draft inference

`scripts/local-speculative.sh` is a space-safe launcher for the GGUF models
already installed by LM Studio. It uses the open-source `llama-server` in
`~/native_tools/llama.cpp`, with Metal offload, Flash Attention, a 4K working
context, one latency-oriented slot, host-buffer bypass, and Q4 KV caches.
The prefill microbatch is 256 tokens, selected from a measured 128/256/512/
1024/2048-token sweep.

It never downloads, copies, converts, or quantizes model weights.

## Recommended setup

Run the automatic lane:

```sh
scripts/local-speculative.sh start-auto
```

The launcher measures uncached prefill, decode, and end-to-end latency for the
Q4_K_M 9B target alone and the 9B + 2B draft pair, then starts only the faster
end-to-end configuration. The speculative lane must be at least 3% faster to
win, avoiding benchmark noise and configurations where draft-model overhead
exceeds accepted-token savings.

Use the OpenAI-compatible endpoint at `http://127.0.0.1:8080/v1`:

```sh
curl http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"local","messages":[{"role":"user","content":"Hello"}]}'
```

For dsco:

```sh
LLAMACPP_API_BASE=http://127.0.0.1:8080/v1 dsco -m llamacpp:local "Hello"
```

Inspect or stop the background server with:

```sh
scripts/local-speculative.sh status
scripts/local-speculative.sh stop
```

## Explicit modes

```sh
scripts/local-speculative.sh serve         # force 9B + 2B draft, foreground
scripts/local-speculative.sh serve-target  # force 9B only, foreground
scripts/local-speculative.sh serve-throughput # four concurrent slots
scripts/local-speculative.sh bench         # compare and exit
scripts/local-speculative.sh print         # show paths and commands only
```

The installed Qwythos 9B and Qwen3.5 2B files have matching Qwen3.5
tokenizers, so they are valid for lossless speculative decoding. Validity does
not guarantee a speedup: acceptance rate and draft cost determine whether the
pair is faster on a particular machine and prompt distribution. `start-auto`
enforces that distinction at runtime.

## Tuning without extra model files

Environment variables keep the default command short:

| Variable | Default | Purpose |
|---|---:|---|
| `DSCO_SPEC_PORT` | `8080` | Loopback server port |
| `DSCO_SPEC_CTX` | `4096` | Allocated working context, not the model maximum |
| `DSCO_SPEC_SLOTS` | `1` | Concurrent slots; one favors interactive latency |
| `DSCO_SPEC_KV_TYPE` | `q4_0` | Compact KV cache type |
| `DSCO_SPEC_BATCH` | `2048` | Logical maximum batch size |
| `DSCO_SPEC_UBATCH` | `256` | Physical prefill chunk size |
| `DSCO_SPEC_DRAFT_MAX` | `4` | Maximum proposed tokens per draft pass |
| `DSCO_SPEC_MIN_GAIN` | `1.03` | Required speculative/baseline speed ratio |
| `DSCO_SPEC_TARGET_MODEL` | Qwythos 9B Q4_K_M path | Existing target GGUF |
| `DSCO_SPEC_DRAFT_MODEL` | Qwen3.5 2B path | Existing draft GGUF |
| `DSCO_LLAMACPP_DIR` | `~/native_tools/llama.cpp/build/bin` | llama.cpp binaries |

On the local M4 Max, the Q4_K_M target averaged 32.77 tokens/s over two
128-token runs versus 23.33 tokens/s for Q8 with identical runtime flags, a
40.5% decode-speed improvement. The Q4_K_M file is also 3.69 GB smaller.

For an uncached 2,981-token prompt, Q4_K_M with a 256-token physical batch
reached 327.07 input tokens/s versus 282.54 tokens/s for Q8. The measured Q4
prefill curve was 268.87/327.07/300.63/242.43/241.28 tokens/s for physical
batches of 128/256/512/1024/2048 respectively.

`serve-throughput` sets four slots, a 16K shared context, unified KV, continuous
batching, and the same 256-token prefill chunks. Keep the default one-slot mode
for the lowest interactive latency; use the throughput mode when requests can
arrive concurrently.

Avoid setting context to the model's advertised 1M maximum for routine use.
KV memory and prefill cost grow with allocated/used context, while most local
agent turns benefit more from an 8K or 16K active window and prefix-cache reuse.
