#!/usr/bin/env python3
#
# /// script
# requires-python = ">=3.10"
# dependencies = [
#   "dspy",
#   "openai",
# ]
# ///
#
"""Run a DSPy CLI with dsco as the LM backend.

dsco's web server exposes an OpenAI-compatible gateway at /v1/chat/completions
(see web/server.py). This script gives you a thin command-line wrapper around
`dspy.Predict`, with dsco doing provider routing behind the OpenAI-compatible
gateway.

    ./dsco --ui 3141 runs the gateway -> dsco provider routing -> Anthropic / OpenAI / OpenRouter / ...

Usage:
    uv run examples/dspy_via_dsco.py "What is the capital of Australia?"
    echo "summarize this" | uv run examples/dspy_via_dsco.py --signature "text -> summary"
"""

import argparse
import json
import socket
import sys
import urllib.error
import urllib.request
from urllib.parse import urlparse


DEFAULT_BASE_URL = "http://127.0.0.1:3141/v1"


def import_dspy():
    try:
        import dspy
    except ModuleNotFoundError as exc:
        if exc.name == "dspy":
            raise SystemExit(
                "Missing dependency: dspy\n"
                "Run this example with `uv run examples/dspy_via_dsco.py ...` so uv installs the script dependencies."
            ) from None
        raise
    return dspy


def split_fields(field_list: str) -> list[str]:
    fields = []
    for raw in field_list.split(","):
        name = raw.strip()
        if not name:
            continue
        # Accept human-friendly fragments such as "verdict: bool"; DSPy's
        # signature parser gets the original string, this is only for IO mapping.
        name = name.split(":", 1)[0].strip()
        name = name.split(None, 1)[0].strip()
        if name:
            fields.append(name)
    return fields


def signature_io_fields(signature: str) -> tuple[list[str], list[str]]:
    if "->" not in signature:
        raise SystemExit(f"Invalid --signature {signature!r}; expected 'input -> output'.")
    inputs, outputs = signature.split("->", 1)
    input_fields = split_fields(inputs)
    output_fields = split_fields(outputs)
    if not input_fields or not output_fields:
        raise SystemExit(f"Invalid --signature {signature!r}; expected at least one input and one output field.")
    return input_fields, output_fields


def parse_field_assignments(assignments: list[str]) -> dict[str, str]:
    values = {}
    for item in assignments:
        name, sep, value = item.partition("=")
        name = name.strip()
        if not sep or not name:
            raise SystemExit(f"Invalid --field {item!r}; expected NAME=VALUE.")
        values[name] = value
    return values


def read_prompt(parts: list[str]) -> str:
    if parts:
        return " ".join(parts).strip()
    if not sys.stdin.isatty():
        return sys.stdin.read().strip()
    return ""


def gateway_models_url(base_url: str) -> str:
    return f"{base_url.rstrip('/')}/models"


def preflight_gateway(base_url: str, timeout: float) -> None:
    """Fail early with a dsco-specific hint when the local gateway is offline."""
    parsed = urlparse(base_url)
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        raise SystemExit(f"Invalid --base-url: {base_url!r}")

    request = urllib.request.Request(
        gateway_models_url(base_url),
        headers={"Authorization": "Bearer dsco", "Accept": "application/json"},
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            # Drain a small response so connection errors surface before DSPy runs.
            response.read(1024)
    except urllib.error.HTTPError:
        # A live OpenAI-compatible endpoint may reject /models but still accept
        # /chat/completions. The important preflight is "server is reachable".
        return
    except (urllib.error.URLError, TimeoutError, socket.timeout, OSError) as exc:
        reason = getattr(exc, "reason", exc)
        raise SystemExit(
            f"Could not reach the dsco OpenAI gateway at {base_url} ({reason}).\n"
            "Start it from the repo root with:\n"
            "  ./dsco --ui 3141\n"
            f"Then retry:\n"
            f"  uv run examples/dspy_via_dsco.py --base-url {DEFAULT_BASE_URL} \"your prompt\""
        ) from None


def prediction_to_dict(prediction, output_fields: list[str]) -> dict[str, object]:
    values = {}
    for name in output_fields:
        if hasattr(prediction, name):
            values[name] = getattr(prediction, name)
    if values:
        return values
    if hasattr(prediction, "toDict"):
        return dict(prediction.toDict())
    if hasattr(prediction, "items"):
        return dict(prediction.items())
    return {"output": str(prediction)}


def main() -> None:
    parser = argparse.ArgumentParser(description="Run a DSPy Predict call through the dsco OpenAI gateway.")
    parser.add_argument("prompt", nargs="*", help="prompt text; stdin is used when omitted")
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL, help="dsco gateway base URL")
    parser.add_argument(
        "--model",
        default="gpt-5.5",
        help="Any model id dsco can route, e.g. gpt-5.5, gpt-5.5-pro, claude-sonnet-5, claude-opus-4-8, claude-fable-5, openrouter/... .",
    )
    parser.add_argument("--signature", default="question -> answer", help="DSPy signature, e.g. 'question -> answer'")
    parser.add_argument("--input-field", default=None, help="field that receives the positional prompt; defaults to the first signature input")
    parser.add_argument("--field", action="append", default=[], metavar="NAME=VALUE", help="set an additional DSPy input field")
    parser.add_argument("--json", action="store_true", help="emit all prediction fields as JSON")
    parser.add_argument("--preflight-timeout", type=float, default=2.0, help="seconds to wait for the gateway preflight")
    parser.add_argument("--debug", action="store_true", help="show the full DSPy/LiteLLM traceback on request failures")
    args = parser.parse_args()

    input_fields, output_fields = signature_io_fields(args.signature)
    prompt = read_prompt(args.prompt)
    inputs = parse_field_assignments(args.field)
    prompt_field = args.input_field or input_fields[0]
    if prompt:
        inputs[prompt_field] = prompt
    missing = [name for name in input_fields if name not in inputs]
    if missing:
        parser.error(f"missing input field(s): {', '.join(missing)}; pass a prompt or --field NAME=VALUE")

    preflight_gateway(args.base_url, args.preflight_timeout)
    dspy = import_dspy()

    lm = dspy.LM(
        model=f"openai/{args.model}",
        api_base=args.base_url,
        api_key="dsco",  # any non-empty value; real creds are resolved inside dsco from the provider's env var
        cache=False,
    )
    dspy.configure(lm=lm)

    try:
        prediction = dspy.Predict(args.signature)(**inputs)
    except Exception as exc:
        if args.debug:
            raise
        raise SystemExit(
            "DSPy request failed through the dsco gateway.\n"
            f"{type(exc).__name__}: {exc}\n\n"
            "Check that the gateway process has provider credentials for this model. "
            "For gateway routing errors, inspect the `./dsco --ui 3141` terminal output. "
            "Use `--debug` to show the full DSPy/LiteLLM traceback."
        ) from None

    values = prediction_to_dict(prediction, output_fields)
    if args.json or len(values) != 1:
        print(json.dumps(values, ensure_ascii=False, indent=2))
    else:
        print(next(iter(values.values())))


if __name__ == "__main__":
    main()
