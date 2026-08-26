#!/usr/bin/env python3
"""Sampler laboratory for OpenAI-compatible local model servers.

Runs server-side sampling configurations and records the returned top-k
logprob distributions.  It also reports an Entropix-style *shadow decision*
from each observed distribution: entropy, selected-vs-runner-up margin, and
which token a policy would choose.  Shadow decisions do not alter server
continuation; changing the next token requires a decoder-level hook.

Examples:
  python3 scripts/local_sampler_lab.py --model qwen35-27b-dense \
      --prompt 'Compute 12345 * 678. Reply only with the integer.'
  python3 scripts/local_sampler_lab.py --model qwen35-27b-dense --suite
"""
import argparse
import json
import math
import sys
import time
import urllib.request

DEFAULT_CASES = [
    ("easy", "Compute 19 + 11. Reply only with the decimal integer."),
    ("carry", "Compute 88888888 + 11111112. Reply only with the decimal integer."),
    ("multiply", "Compute 12345 * 678. Reply only with the decimal integer."),
    ("modulo", "Compute the remainder when 123456789 is divided by 97. Reply only with the decimal integer."),
]
CONFIGS = [
    ("greedy", {"temperature": 0}),
    ("temp_0_35", {"temperature": 0.35, "seed": 1701}),
    ("temp_0_70", {"temperature": 0.70, "seed": 1701}),
    ("temp_1_00", {"temperature": 1.0, "seed": 1701}),
    ("nucleus_0_90", {"temperature": 0.70, "top_p": 0.90, "seed": 1701}),
]

def request(endpoint, model, prompt, sampling, topk):
    body = {
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
        "stream": False,
        "max_tokens": 96,
        "logprobs": True,
        "top_logprobs": topk,
        **sampling,
    }
    # Qwen 3.5 honors this. Other local servers/models may ignore or reject it.
    if model.startswith("qwen35-"):
        body["reasoning_effort"] = "none"
    req = urllib.request.Request(endpoint, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    start = time.perf_counter()
    with urllib.request.urlopen(req, timeout=180) as response:
        payload = json.load(response)
    return payload, time.perf_counter() - start

def metrics(entries):
    rows = []
    for entry in entries:
        alternatives = entry.get("top_logprobs") or []
        probs = [math.exp(x["logprob"]) for x in alternatives]
        mass = sum(probs)
        # This is entropy over returned candidates; mass is emitted so callers
        # know whether the top-k approximation is adequate.
        entropy = -sum(p * math.log(p) for p in probs if p > 0)
        selected = entry.get("token")
        runner = next((x for x in alternatives if x.get("token") != selected), None)
        chosen = next((x for x in alternatives if x.get("token") == selected), None)
        rows.append({
            "token": selected,
            "selected_logprob": entry.get("logprob"),
            "topk_probability_mass": mass,
            "topk_entropy_nats": entropy,
            "margin_to_runner_up_nats": (
                entry["logprob"] - runner["logprob"] if runner else None),
            "shadow_argmax_token": alternatives[0]["token"] if alternatives else None,
            "shadow_argmax_differs": bool(alternatives and alternatives[0]["token"] != selected),
        })
    return rows

def run_case(endpoint, model, case_name, prompt, sampling_name, sampling, topk):
    payload, seconds = request(endpoint, model, prompt, sampling, topk)
    choice = payload["choices"][0]
    message = choice.get("message", {})
    entries = (choice.get("logprobs") or {}).get("content", [])
    token_metrics = metrics(entries)
    return {
        "case": case_name,
        "sampler": sampling_name,
        "sampling": sampling,
        "answer": message.get("content", ""),
        "reasoning_chars": len(message.get("reasoning_content") or ""),
        "finish_reason": choice.get("finish_reason"),
        "seconds": round(seconds, 3),
        "usage": payload.get("usage", {}),
        "token_metrics": token_metrics,
        "max_entropy_nats": max((x["topk_entropy_nats"] for x in token_metrics), default=None),
        "min_margin_nats": min((x["margin_to_runner_up_nats"] for x in token_metrics
                                  if x["margin_to_runner_up_nats"] is not None), default=None),
    }

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--endpoint", default="http://127.0.0.1:1234/v1/chat/completions")
    parser.add_argument("--model", required=True)
    parser.add_argument("--prompt")
    parser.add_argument("--suite", action="store_true")
    parser.add_argument("--top-logprobs", type=int, default=20)
    parser.add_argument("--output", help="write JSONL results to this path")
    args = parser.parse_args()
    if not args.suite and not args.prompt:
        parser.error("provide --prompt or --suite")
    cases = DEFAULT_CASES if args.suite else [("custom", args.prompt)]
    output = open(args.output, "w") if args.output else None
    ok = 0
    for name, prompt in cases:
        for sampler_name, sampling in CONFIGS:
            try:
                result = run_case(args.endpoint, args.model, name, prompt, sampler_name,
                                  sampling, args.top_logprobs)
                ok += 1
            except Exception as exc:
                result = {"case": name, "sampler": sampler_name, "error": str(exc)}
            encoded = json.dumps(result, ensure_ascii=False)
            print(encoded)
            if output:
                output.write(encoded + "\n")
                output.flush()
    if output:
        output.close()
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
