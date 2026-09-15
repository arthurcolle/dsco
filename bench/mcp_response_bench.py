#!/usr/bin/env python3
"""Paired live-binary MCP read responses with byte-equivalence verification."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import tempfile

from warm_mcp_compare import Host, percentile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--requests", type=int, default=20)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    binaries = {"before": args.before.resolve(), "after": args.after.resolve()}
    env = os.environ.copy()
    env.update(DSCO_GOV_MODEL="none", DSCO_TOOLMGMT="0", DSCO_MCP_HEADLESS="0",
               DSCO_NO_KEYCHAIN="1", DSCO_READ_DEDUP="0",
               DSCO_NO_WORKSPACE_BOOTSTRAP="1")
    for key in ["DSCO_GOV_BYPASS", "DSCO_DURABLE_AGENT_ID", "DSCO_SUBAGENT"]:
        env.pop(key, None)
    rows, starts, catalog_checks = [], [], []
    with tempfile.TemporaryDirectory(prefix="dsco-response-bench-") as temp:
        cwd = Path(temp)
        fixtures = {
            "small": "simple output line\n" * 4,
            "plain_256k": ("abcdefghijklmnopqrstuvwxyz0123456789 " * 3 + "\n") * 2341,
            "code_256k": ('if (ready) { emit("value", path); } // 世界\n' * 5900),
        }
        for name, content in fixtures.items():
            (cwd / (name + ".txt")).write_text(content)
        for session in range(2):
            hosts = {}
            try:
                for name in (["before", "after"] if session == 0 else ["after", "before"]):
                    host = Host(name, [str(binaries[name]), "--gov-model", "none",
                                       "mcp", "serve", "--toolsets", "all", "--tier", "trusted"],
                                cwd, env, out, session)
                    hosts[name] = host
                    starts.append({"session": session, "variant": name,
                                   "startup_ms": host.startup_ms, "catalog_ms": host.catalog_ms})
                assert hosts["before"].tools == hosts["after"].tools
                catalog_checks.append(len(hosts["after"].tools))
                for case in fixtures:
                    for n in range(args.requests + 2):
                        responses = {}
                        for name in (["before", "after"] if n % 2 == 0 else ["after", "before"]):
                            response, ms = hosts[name].call("tools/call", {
                                "name": "read_file", "arguments": {"path": case + ".txt"}})
                            result = response["result"]
                            text = result["content"][0]["text"]
                            assert not result["isError"] and "unchanged since" not in text
                            assert fixtures[case].splitlines()[0] in text
                            responses[name] = result
                            if n >= 2:
                                rows.append({"session": session, "case": case, "variant": name,
                                             "elapsed_ms": ms, "result_chars": len(text),
                                             "result_utf8_bytes": len(text.encode()),
                                             "truncated": "[truncated " in text,
                                             "result_sha256": hashlib.sha256(text.encode()).hexdigest()})
                        assert responses["before"] == responses["after"], (case, n)
            finally:
                for host in hosts.values():
                    host.close()
    summaries = []
    for case in fixtures:
        entry = {"case": case, "input_file_bytes": len(fixtures[case].encode()),
                 "result_utf8_bytes": sorted({r["result_utf8_bytes"] for r in rows if r["case"] == case}),
                 "uses_existing_result_truncation": any(r["truncated"] for r in rows if r["case"] == case)}
        for name in binaries:
            samples = [r["elapsed_ms"] for r in rows if r["case"] == case and r["variant"] == name]
            entry[name] = {"n": len(samples), "median_ms": statistics.median(samples),
                           "p95_ms": percentile(samples, .95)}
        entry["median_reduction_percent"] = 100 * (1 - entry["after"]["median_ms"] / entry["before"]["median_ms"])
        summaries.append(entry)
    result = {"scope": "complete warm MCP request including pipe transfer and Python JSON decode; no inference",
              "binary_sha256": {k: hashlib.sha256(p.read_bytes()).hexdigest() for k, p in binaries.items()},
              "environment_overrides": {k: env[k] for k in ["DSCO_GOV_MODEL", "DSCO_READ_DEDUP", "DSCO_TOOLMGMT"]},
              "identical_catalog_counts": catalog_checks, "byte_equal_results": True,
              "timings": summaries, "starts": starts}
    (out / "records.json").write_text(json.dumps(rows, indent=2) + "\n")
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
