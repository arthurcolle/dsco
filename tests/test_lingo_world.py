#!/usr/bin/env python3
"""Native process conformance for source-bound worlds and value addresses."""
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "dsco").resolve()
REPORT = Path(sys.argv[2]) if len(sys.argv) > 2 else None
SOURCE = (ROOT / "tests/lingo_world.lingo").read_text()
ENV = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "DSCO_ENV_FILE": "/dev/null",
       "DSCO_PRICING_OFFLINE": "1", "DSCO_SECURE_STORE_NO_PROMPT": "1",
       "DSCO_GOV_MODEL": "standard", "DSCO_NO_AUTO_SUPERVISE": "1",
       "DSCO_ALLOW_NET": "0", "DSCO_TOOLMGMT": "0", "DSCO_NO_COLOR": "1"}
CHECKS = []


def invoke(source=SOURCE, args=None, success=True):
    with tempfile.TemporaryDirectory(prefix="lingo-world-") as directory:
        process = subprocess.run([str(BINARY), "lingo", "eval", source, json.dumps(args or {})],
                                 cwd=directory, env=ENV, capture_output=True, text=True, timeout=25)
    assert (process.returncode == 0) == success, (process.returncode, process.stdout, process.stderr)
    receipt = json.loads(process.stdout)
    if success:
        assert receipt["api"] == "0.2" and receipt["tool_calls"] == 0, receipt
        assert receipt["source_sha256"] == hashlib.sha256(source.encode()).hexdigest()
        return receipt["value"]
    return receipt


def check(name, condition):
    assert condition, name
    CHECKS.append(name)
    print("PASS", name)


saved = invoke(args={"mode": "save"})
snapshot, address = saved["snapshot"], saved["address"]
restored = invoke(args={"mode": "load", **saved})
check("fresh process restores exact stored values, revisions and reference cycles", restored["snapshot"] == snapshot)
check("derived values are recalculated and caches are never loaded", restored["before"] == "unread" and restored["value"] == 11)
check("logical address tracks base invalidation", restored["after"] == 13 and restored["why"]["evaluations"] == 2)
check("logical address resolves active hypothetical context", saved["hypothetical"] == 19 and saved["baseline"] == 11)
check("reference-valued calculation arguments serialize by typed object identity", address["args"] == {"factor": 2, "other": "z"})
check("definition source is exact program identity", snapshot["classes"]["Bundle"]["source"]["sha256"] == hashlib.sha256(SOURCE.encode()).hexdigest())
check("derived functions carry prototype fingerprints", len(snapshot["classes"]["Bundle"]["fields"]["scaled"]["source"]["prototype_sha256"]) == 64)
check("false defaults survive manifest construction", snapshot["classes"]["Bundle"]["fields"]["enabled"]["default"] is False)


def rejects(name, mutate, message):
    changed = copy.deepcopy(snapshot)
    mutate(changed)
    result = invoke(args={"snapshot": changed, "address": address})
    check(name, result.get("loaded") is False and result["count"] == 0 and message in result["error"])


rejects("wrong world fails before hydration", lambda s: s.update(world_id="different"), "world identity")
rejects("different runtime fails before hydration", lambda s: s["runtime"].update(api="999"), "runtime identity")
rejects("different compiler fails before hydration", lambda s: s["runtime"].update(compiler="different"), "runtime identity")
rejects("changed definition source fails before hydration", lambda s: s["classes"]["Link"]["source"].update(sha256="0" * 64), "definition or source")
rejects("changed derived prototype fails before hydration", lambda s: s["classes"]["Bundle"]["fields"]["scaled"]["source"].update(prototype_sha256="0" * 64), "definition or source")
rejects("extra snapshot fields are rejected", lambda s: s.update(ignored=True), "unknown field")
rejects("duplicate object IDs leave world empty", lambda s: s["objects"].append(s["objects"][0]), "duplicate")
rejects("wrong stored type leaves world empty", lambda s: s["objects"][0]["values"].update(amount="3"), "exact integer")
rejects("dangling reference leaves world empty", lambda s: s["objects"][0]["values"].update(next="missing"), "missing or wrongly typed")
rejects("wrong reference class leaves world empty", lambda s: s["objects"][0]["values"].update(next="bundle"), "missing or wrongly typed")
rejects("unknown stored fields leave world empty", lambda s: s["objects"][0]["values"].update(unknown=1), "unknown field")
rejects("missing stored fields leave world empty", lambda s: s["objects"][0]["values"].pop("amount"), "missing amount")
rejects("invalid revision leaves world empty", lambda s: s["objects"][0].update(revision=0), "revision")
rejects("object metadata is strict", lambda s: s["objects"][0].update(cache={}), "unknown field")
rejects("empty object list must remain a JSON array", lambda s: s.update(objects={}), "explicit empty array")
rejects("empty typed list must remain a JSON array", lambda s: s["objects"][1]["values"].update(links={}), "explicit empty array")
maximum = copy.deepcopy(snapshot)
maximum["objects"][0]["revision"] = 2**53 - 1
check("revision exhaustion is rejected before mutation", invoke(args={"mode": "revision_limit", "snapshot": maximum, "address": address})["unchanged"])
changed_program = invoke(source=SOURCE + "\n-- changed source\n", args={"snapshot": snapshot, "address": address})
check("changed entry program cannot impersonate original custom definitions", changed_program["loaded"] is False)

for field, value, message in [("world_id", "elsewhere", "different world"),
                               ("definition_sha256", "0" * 64, "definition identity"),
                               ("class", "Link", "class differs")]:
    invalid = copy.deepcopy(address)
    invalid[field] = value
    # Resolve is also used by explain, so either must reject the same address.
    result = invoke(args={"mode": "invalid_address", "snapshot": snapshot, "address": invalid}, success=False)
    check("address rejects changed " + field, message in result["error"])

for expression, message in [("l.world(false)", "record"), ("l.world{id=false}", "world ID"),
                            ('l.world{id=string.rep("x",129)}', "world ID"),
                            ('w:define("X",{x=l.derived("number",math.abs)})', "native functions")]:
    source = 'local l=require("lingo");local w=l.world();' + expression
    result = invoke(source=source, success=False)
    check("invalid declaration is recoverable: " + expression, message in result["error"])

for default in ('l.stored(l.types.ref("X"),x)',
                'l.stored(l.types.list(l.types.ref("X")),{x})'):
    source = ('local l=require("lingo");local w=l.world();w:define("X",{n=l.stored("integer",1)});'
              'local x=w:new("X","x",{});w:define("Y",{ref=' + default + '});return w:manifest()')
    check("portable definitions reject concrete reference default: " + default,
          "reference defaults" in invoke(source=source, success=False)["error"])
for invalid_args in ('false', 'l.array{}'):
    source = ('local l=require("lingo");local w=l.world();w:define("X",{n=l.stored("integer",1)});'
              'local x=w:new("X","x",{});return w:address(x,"n",' + invalid_args + ')')
    check("argument records reject " + invalid_args, "record" in invoke(source=source, success=False)["error"])

for declaration, message in [
    ('w:define("X",{});w:new("X","bad\\nidentity",{});return w:snapshot()', "portable object ID"),
    ('w:define(string.rep("x",1025),{});return w:manifest()', "portable class name"),
]:
    source='local l=require("lingo");local w=l.world();'+declaration
    check("portable identities reject " + message, message in invoke(source=source, success=False)["error"])

report = {"status": "PASS", "binary": str(BINARY), "binary_sha256": hashlib.sha256(BINARY.read_bytes()).hexdigest(),
          "passed": len(CHECKS), "checks": CHECKS,
          "scope": "Fresh native CLI processes, no service or provider calls"}
if REPORT:
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
