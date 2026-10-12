#!/usr/bin/env python3
"""Exercise embedded platform objects in the real DSCO binary, offline."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", default=str(ROOT / "dsco"))
    parser.add_argument("--report", type=Path)
    options = parser.parse_args()
    binary = str(Path(options.binary).resolve(strict=True))
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "DSCO_ENV_FILE": "/dev/null",
           "DSCO_PRICING_OFFLINE": "1", "DSCO_SECURE_STORE_NO_PROMPT": "1",
           "DSCO_GOV_MODEL": "standard", "DSCO_NO_AUTO_SUPERVISE": "1",
           "DSCO_ALLOW_NET": "0", "DSCO_TOOLMGMT": "0", "DSCO_NO_COLOR": "1"}
    with tempfile.TemporaryDirectory(prefix="dsco-platform-") as directory:
        def invoke(source, args=None, path=False, success=True):
            command = [binary, "lingo", "run" if path else "eval", str(source)]
            if args is not None:
                command.append(json.dumps(args))
            process = subprocess.run(command, cwd=directory, env=env, capture_output=True,
                                     text=True, timeout=25)
            assert (process.returncode == 0) == success, (process.returncode, process.stdout, process.stderr)
            return json.loads(process.stdout)

        result = invoke(ROOT / "tests/lingo_platform.lingo", path=True)
        assert result["tool_calls"] == 0, result
        value = result["value"]
        fresh = invoke('local p=require("lingo.platform");local w=p.world{id="platform-semantics"};'
                       'w:load(args.snapshot);return w:read(args.address)',
                       {"snapshot": value["snapshot"], "address": value["address"]})
        assert fresh["tool_calls"] == 0 and fresh["value"] == value["summary"], fresh
        checks = value["checks"] + ["fresh process reconstructs the same typed calculation with zero tool calls"]
        # A platform planning decision cannot suppress the existing NET denial.
        denied = invoke('local p=require("lingo.platform");local l=require("lingo");'
                        'local w=p.world{id="scope"};local policy=w:new("Policy","p",'
                        '{allow_effects=true,max_cost_usd=1,max_calls=1});'
                        'local r=l.call("graphsub_operator",{action="status"});'
                        'assert(not r.ok and r.result:find("DSCO_ALLOW_NET",1,true),r.result);return policy.allow_effects')
        assert denied["value"] is True
        checks.append("advisory Policy cannot grant a denied native capability")

    report = {"status": "PASS", "binary": binary, "passed": len(checks), "checks": checks,
              "scope": "Real embedded Lingo runtime; synthetic labeled observations, no provider/network/backend effects"}
    if options.report:
        options.report.parent.mkdir(parents=True, exist_ok=True)
        options.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
