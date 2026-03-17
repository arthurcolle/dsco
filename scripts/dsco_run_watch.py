#!/usr/bin/env python3
"""Watch a DSCO run over SSE."""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.parse
import urllib.request


def iter_sse_lines(response):
    while True:
        line = response.readline()
        if not line:
            break
        yield line.decode("utf-8", errors="replace").rstrip("\n")


def watch_run(base_url: str, run_id: str, api_key: str | None) -> int:
    target = base_url.rstrip("/") + f"/ds/v1/runs/{urllib.parse.quote(run_id)}/events/stream"
    request = urllib.request.Request(
        target,
        headers={
            "Accept": "text/event-stream",
            **({"X-API-Key": api_key} if api_key else {}),
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=3600) as response:
            event_name = "message"
            data_lines: list[str] = []
            for line in iter_sse_lines(response):
                if not line:
                    if data_lines:
                        raw = "\n".join(data_lines)
                        try:
                            parsed = json.loads(raw)
                            payload = json.dumps(
                                {"event": event_name, "data": parsed},
                                ensure_ascii=True,
                            )
                        except json.JSONDecodeError:
                            payload = json.dumps(
                                {"event": event_name, "data": raw},
                                ensure_ascii=True,
                            )
                        print(payload, flush=True)
                    event_name = "message"
                    data_lines = []
                    continue
                if line.startswith(":"):
                    continue
                if line.startswith("event:"):
                    event_name = line.split(":", 1)[1].strip() or "message"
                    continue
                if line.startswith("data:"):
                    data_lines.append(line.split(":", 1)[1].lstrip())
            return 0
    except urllib.error.HTTPError as exc:
        sys.stderr.write(f"HTTP {exc.code}: {exc.reason}\n")
        return 1
    except Exception as exc:
        sys.stderr.write(f"{exc}\n")
        return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Watch a DSCO run event stream.")
    parser.add_argument("run_id")
    parser.add_argument("--base-url", default="http://127.0.0.1:8031")
    parser.add_argument("--api-key", default=None)
    args = parser.parse_args()
    return watch_run(args.base_url, args.run_id, args.api_key)


if __name__ == "__main__":
    raise SystemExit(main())
