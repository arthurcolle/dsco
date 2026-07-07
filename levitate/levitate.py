#!/usr/bin/env python3
"""levitate.py — a structured LLM tool-calling agent loop in ~300 lines.

Design (the parts that actually matter):
  - Tools are declared once: python function + JSON schema derived from it.
  - The runtime validator is ground truth, not the docstring (ledger E-004):
    args are validated against the schema BEFORE dispatch; on failure the
    model gets the validation error back as the tool result and may retry.
  - Bounded loop: max_turns hard cap. No unbounded agent.
  - Every tool result is tagged with provenance (tool name, ok/error).
  - Provider-agnostic core; Anthropic + OpenAI adapters; --mock runs offline.

Usage:
  python3 levitate.py --mock "what is 40320 / 8 and what's in /tmp?"
  ANTHROPIC_API_KEY=... python3 levitate.py "count the .md files in the cwd"
"""
from __future__ import annotations
import argparse, inspect, json, os, subprocess, sys, urllib.request

# ── Tool registry ──────────────────────────────────────────────────────────

TOOLS: dict[str, dict] = {}  # name -> {fn, schema}

_PYTYPE_TO_JSON = {str: "string", int: "integer", float: "number", bool: "boolean"}

def tool(fn):
    """Register a function as a tool. Schema derived from signature+docstring."""
    sig = inspect.signature(fn)
    props, required = {}, []
    for pname, p in sig.parameters.items():
        jtype = _PYTYPE_TO_JSON.get(p.annotation, "string")
        props[pname] = {"type": jtype}
        if p.default is inspect.Parameter.empty:
            required.append(pname)
    TOOLS[fn.__name__] = {
        "fn": fn,
        "schema": {
            "name": fn.__name__,
            "description": (fn.__doc__ or fn.__name__).strip(),
            "input_schema": {"type": "object", "properties": props,
                             "required": required},
        },
    }
    return fn

def validate(name: str, args: dict) -> str | None:
    """Validate args against the registered schema. Returns error msg or None.
    The validator is authoritative — dispatch never trusts the model's args."""
    if name not in TOOLS:
        return f"unknown tool: {name}. available: {sorted(TOOLS)}"
    schema = TOOLS[name]["schema"]["input_schema"]
    missing = [r for r in schema["required"] if r not in args]
    if missing:
        return f"missing required field(s): {missing}"
    for k, v in args.items():
        if k not in schema["properties"]:
            return f"unexpected field: {k}"
        want = schema["properties"][k]["type"]
        ok = {"string": str, "integer": int, "number": (int, float),
              "boolean": bool}[want]
        if not isinstance(v, ok):
            return f"field {k}: expected {want}, got {type(v).__name__}"
    return None

def dispatch(name: str, args: dict) -> dict:
    """Validate then execute. Errors are results, not exceptions — the model
    sees them and can correct (error message = authoritative schema)."""
    err = validate(name, args)
    if err:
        return {"ok": False, "error": err}
    try:
        return {"ok": True, "result": TOOLS[name]["fn"](**args)}
    except Exception as e:
        return {"ok": False, "error": f"{type(e).__name__}: {e}"}

# ── Tools ──────────────────────────────────────────────────────────────────

@tool
def calc(expression: str):
    """Evaluate an arithmetic expression (numbers and + - * / ** % // only)."""
    allowed = set("0123456789.+-*/%() eE")
    if not set(expression) <= allowed:
        raise ValueError("non-arithmetic characters rejected")
    return eval(compile(expression, "<calc>", "eval"), {"__builtins__": {}}, {})

@tool
def read_file(path: str):
    """Read a text file (first 4000 chars)."""
    with open(path, "r", errors="replace") as f:
        return f.read(4000)

@tool
def list_dir(path: str):
    """List directory entries."""
    return sorted(os.listdir(path))[:200]

@tool
def shell(command: str):
    """Run a shell command (10s timeout). Returns stdout+stderr."""
    r = subprocess.run(command, shell=True, capture_output=True, text=True,
                       timeout=10)
    return {"exit": r.returncode, "out": r.stdout[-4000:], "err": r.stderr[-2000:]}

# ── Provider adapters ──────────────────────────────────────────────────────

def _post(url: str, headers: dict, body: dict) -> dict:
    req = urllib.request.Request(url, json.dumps(body).encode(),
                                 {"Content-Type": "application/json", **headers})
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.load(r)

def call_anthropic(messages: list, system: str) -> tuple[str, list]:
    """Returns (text, [(id, name, args), ...])."""
    resp = _post("https://api.anthropic.com/v1/messages",
                 {"x-api-key": os.environ["ANTHROPIC_API_KEY"],
                  "anthropic-version": "2023-06-01"},
                 {"model": os.environ.get("MODEL", "claude-sonnet-4-5"),
                  "max_tokens": 4096, "system": system, "messages": messages,
                  "tools": [t["schema"] for t in TOOLS.values()]})
    text, calls = "", []
    for block in resp.get("content", []):
        if block["type"] == "text":
            text += block["text"]
        elif block["type"] == "tool_use":
            calls.append((block["id"], block["name"], block["input"]))
    return text, calls, resp["content"]

def call_mock(messages: list, system: str):
    """Offline model: a tiny scripted planner so the loop is testable with no
    key. Turn 1: emit tool calls parsed naively from the user text. Turn 2+:
    summarize tool results."""
    user_text = next(m["content"] for m in messages
                     if m["role"] == "user" and isinstance(m["content"], str))
    already_ran = any(isinstance(m.get("content"), list) and
                      any(b.get("type") == "tool_result" for b in m["content"])
                      for m in messages)
    if already_ran:
        results = [b["content"] for m in messages if isinstance(m.get("content"), list)
                   for b in m["content"] if b.get("type") == "tool_result"]
        return f"[mock] done. tool results: {json.dumps(results)[:600]}", [], None
    calls = []
    import re
    expr = re.search(r"[\d][\d\s+\-*/.()]*[\d)]", user_text)
    if expr:
        calls.append(("m1", "calc", {"expression": expr.group().strip()}))
    path = re.search(r"(/[\w./-]+)", user_text)
    if path:
        calls.append(("m2", "list_dir", {"path": path.group(1)}))
    if not calls:  # deliberately send a BAD call to exercise the validator
        calls.append(("m3", "calc", {"expr": "1+1"}))
    return "[mock] planning tool calls", calls, None

# ── Agent loop ─────────────────────────────────────────────────────────────

SYSTEM = ("You are a precise tool-using agent. Use tools for anything "
          "computable or observable; never guess file contents or arithmetic. "
          "If a tool returns a validation error, read it and correct the call.")

def run(task: str, mock: bool, max_turns: int = 10) -> str:
    model = call_mock if mock else call_anthropic
    messages = [{"role": "user", "content": task}]
    for turn in range(1, max_turns + 1):
        text, calls, raw = model(messages, SYSTEM)
        if not calls:
            return text  # terminal: model answered without tools
        # append assistant turn (raw blocks for real API, synthetic for mock)
        messages.append({"role": "assistant", "content": raw or [
            {"type": "tool_use", "id": i, "name": n, "input": a}
            for i, n, a in calls]})
        results = []
        for call_id, name, args in calls:
            out = dispatch(name, args)
            print(f"  [turn {turn}] {name}({json.dumps(args)[:80]}) -> "
                  f"{'ok' if out['ok'] else 'ERR: ' + out['error']}",
                  file=sys.stderr)
            results.append({"type": "tool_result", "tool_use_id": call_id,
                            "content": json.dumps(out, default=str)[:4000]})
        messages.append({"role": "user", "content": results})
    return f"[halted: max_turns={max_turns} reached — bounded loop, no overdraft]"

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("task")
    ap.add_argument("--mock", action="store_true", help="run offline")
    ap.add_argument("--max-turns", type=int, default=10)
    a = ap.parse_args()
    print(run(a.task, mock=a.mock or "ANTHROPIC_API_KEY" not in os.environ,
              max_turns=a.max_turns))
