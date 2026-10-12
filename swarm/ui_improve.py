#!/usr/bin/env python3
"""UI self-improvement hook — RSI outer loop for the console frontend.

Cycle:
  1. CAPTURE  N sequential frames of the live console (Chrome headless with
              increasing --virtual-time-budget, so the SMIL/CSS animation
              timeline is sampled at successive instants).
  2. REVIEW   a vision lane sees the frames (labeled with timestamps) plus the
              full index.html source and proposes minimal find/replace patches
              (animation continuity, hierarchy, legibility, wasted space).
  3. APPLY    one patch at a time. Each application is gated: the patched page
              must still render — headless --dump-dom must contain the key
              live element IDs with populated content — else instant rollback.
  4. LEDGER   every version of index.html is retained under static/ui_versions/.

    uv run --with openai python ui_improve.py --frames 4 --max-apply 3
Env: UI_VISION_BASE_URL (default OpenRouter), UI_VISION_MODEL (openai/gpt-5.2),
     OPENROUTER_API_KEY, SWARM_CONSOLE_URL (default http://127.0.0.1:8422).
"""

import argparse
import base64
import json
import os
import re
import shutil
import subprocess
import time
from pathlib import Path

import openai

HERE = Path(__file__).parent
INDEX = HERE / "static" / "index.html"
VERSIONS = HERE / "static" / "ui_versions"
CHROME = os.getenv("CHROME_BIN", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")
URL = os.getenv("SWARM_CONSOLE_URL", "http://127.0.0.1:8422")
BASE_URL = os.getenv("UI_VISION_BASE_URL", "https://openrouter.ai/api/v1")
MODEL = os.getenv("UI_VISION_MODEL", "openai/gpt-5.2")
KEY = os.getenv("UI_VISION_API_KEY") or os.getenv("OPENROUTER_API_KEY", "")
# IDs that must exist AND be populated after JS runs — the render gate.
REQUIRED_IDS = ("tiles", "arch", "board", "throughput", "table")

REVIEW_PROMPT = """You are a senior UI engineer reviewing a live ops console (dark, industrial,
IBM Plex Mono + Big Shoulders Display). The images are SEQUENTIAL FRAMES of the same page at
t={times}s — differences between frames show the animation (SMIL particles, marching-ants
dashes, pulsing rings).

Review for: animation continuity/legibility, visual hierarchy, alignment, wasted space,
label collisions, contrast. Respect the existing aesthetic; no framework changes; no new
dependencies; keep the validated palette variables.

Below is the COMPLETE index.html. Propose up to {n} minimal, independent improvements as
exact patches. Reply ONLY with JSON:
{{"improvements":[{{"title":"...","rationale":"...","find":"<EXACT substring from the file>","replace":"<replacement>"}}]}}
Each "find" must appear VERBATIM and exactly once. Keep each patch small and self-contained.

```html
{src}
```"""


def shoot(path: Path, budget_ms: int, size="1480,900"):
    subprocess.run([CHROME, "--headless", "--disable-gpu", f"--screenshot={path}",
                    f"--window-size={size}", "--hide-scrollbars",
                    f"--virtual-time-budget={budget_ms}", URL],
                   capture_output=True, timeout=90)
    return path.exists() and path.stat().st_size > 20_000


def capture_frames(n: int, start_ms=3000, step_ms=1300) -> list[tuple[float, Path]]:
    out = []
    for i in range(n):
        budget = start_ms + i * step_ms
        p = Path(f"/tmp/ui_frame_{i}.png")
        if shoot(p, budget):
            out.append((budget / 1000, p))
    return out


def render_gate() -> tuple[bool, str]:
    """Page must render with populated live sections after JS executes."""
    r = subprocess.run([CHROME, "--headless", "--disable-gpu", "--dump-dom",
                        "--virtual-time-budget=6000", URL],
                       capture_output=True, text=True, timeout=90)
    dom = r.stdout or ""
    if len(dom) < 5000:
        return False, f"dom too small ({len(dom)}b)"
    for rid in REQUIRED_IDS:
        m = re.search(rf'id="{rid}"[^>]*>(.{{0,40}})', dom, re.S)
        if not m:
            return False, f"#{rid} missing"
        if not m.group(1).strip():
            return False, f"#{rid} empty (js failed?)"
    return True, "dom ok"


def review(frames, src, n_patches: int):
    content = [{"type": "text", "text": REVIEW_PROMPT.format(
        times=",".join(f"{t:.1f}" for t, _ in frames), n=n_patches, src=src)}]
    for t, p in frames:
        b64 = base64.b64encode(p.read_bytes()).decode()
        content.append({"type": "text", "text": f"Frame at t={t:.1f}s:"})
        content.append({"type": "image_url", "image_url": {"url": f"data:image/png;base64,{b64}"}})
    client = openai.OpenAI(base_url=BASE_URL, api_key=KEY, timeout=300)
    r = client.chat.completions.create(model=MODEL, max_tokens=8000,
                                       messages=[{"role": "user", "content": content}])
    text = r.choices[0].message.content or ""
    for fence in re.findall(r"```(?:json)?\s*([\s\S]*?)```", text) + [text]:
        try:
            d = json.loads(fence)
            if isinstance(d, dict) and d.get("improvements"):
                return d["improvements"]
        except Exception:
            continue
    m = re.search(r"\{[\s\S]*\"improvements\"[\s\S]*\}", text)
    if m:
        try:
            return json.loads(m.group(0))["improvements"]
        except Exception:
            pass
    return []


def snapshot(tag: str) -> Path:
    VERSIONS.mkdir(exist_ok=True)
    n = len(list(VERSIONS.glob("ui*.html"))) + 1
    dst = VERSIONS / f"ui{n:04d}_{tag}.html"
    shutil.copy2(INDEX, dst)
    return dst


def ledger(entry: dict):
    lp = VERSIONS / "ledger.jsonl"
    entry["ts"] = time.time()
    with open(lp, "a") as f:
        f.write(json.dumps(entry) + "\n")


def cycle(frames_n: int, max_apply: int) -> list[dict]:
    print(f"[ui] capturing {frames_n} animation frames…")
    frames = capture_frames(frames_n)
    if len(frames) < 2:
        raise SystemExit("could not capture frames — is the console up?")
    src = INDEX.read_text()
    print(f"[ui] reviewing via {MODEL} ({len(frames)} frames, {len(src)//1024}KB source)…")
    patches = review(frames, src, max_apply + 2)
    print(f"[ui] {len(patches)} improvement(s) proposed")
    results = []
    applied = 0
    for p in patches:
        if applied >= max_apply:
            results.append({"title": p.get("title"), "outcome": "deferred"})
            continue
        title, f, r = p.get("title", "untitled"), p.get("find", ""), p.get("replace", "")
        cur = INDEX.read_text()
        if not f or cur.count(f) != 1:
            results.append({"title": title, "outcome": "no_verbatim_match"})
            ledger({"title": title, "outcome": "no_verbatim_match"})
            continue
        backup = snapshot("pre")
        INDEX.write_text(cur.replace(f, r))
        ok, detail = render_gate()
        if ok:
            applied += 1
            snapshot("applied")
            results.append({"title": title, "outcome": "applied", "rationale": p.get("rationale", "")})
            ledger({"title": title, "outcome": "applied", "rationale": p.get("rationale", "")})
            print(f"  [applied] {title}")
        else:
            shutil.copy2(backup, INDEX)
            results.append({"title": title, "outcome": f"rolled_back ({detail})"})
            ledger({"title": title, "outcome": "rolled_back", "detail": detail})
            print(f"  [rolled back] {title} — {detail}")
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=4)
    ap.add_argument("--max-apply", type=int, default=3)
    ap.add_argument("--cycles", type=int, default=1)
    args = ap.parse_args()
    if not KEY:
        raise SystemExit("set OPENROUTER_API_KEY (or UI_VISION_API_KEY)")
    for i in range(args.cycles):
        print(f"══ UI cycle {i+1}/{args.cycles} ══")
        for r in cycle(args.frames, args.max_apply):
            print(f"  · {r['outcome']}: {r['title']}")


if __name__ == "__main__":
    main()
