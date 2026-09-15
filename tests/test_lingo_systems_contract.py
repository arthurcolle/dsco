#!/usr/bin/env python3
"""Capture the real MCP advertisement and validate Lingo systems contracts.

Run with /usr/bin/python3 when that interpreter provides jsonschema. No backend
requests are made: tools/list and two invalid tools/call inputs exercise MCP;
successful native outputs come from a prior actual-services test report.
"""
import argparse
import copy
from datetime import datetime, timezone
import hashlib
from importlib.metadata import version
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

from jsonschema import Draft202012Validator


ROOT = Path(__file__).resolve().parents[1]
TOOLS = {"autobot_discover": "lingo_autobot", "chimera_route": "lingo_chimera"}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_schema(path, symbol):
    # Parse only adjacent C string literals, never execute source text.
    source = path.read_text()
    match = re.search(r"const char " + re.escape(symbol) +
                      r'\[\]\s*=\s*((?:"(?:[^"\\]|\\.)*"\s*)+);', source)
    if not match:
        raise ValueError("schema string constant not found: " + symbol)
    strings = re.findall(r'"(?:[^"\\]|\\.)*"', match.group(1))
    return json.loads("".join(json.loads(s) for s in strings))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", type=Path, default=ROOT / "dsco")
    parser.add_argument("--live-report", type=Path,
                        default=ROOT / "reports/lingo-systems-20260909/reproducible-services/systems-services-tests.json")
    parser.add_argument("--report", type=Path,
                        default=ROOT / "reports/lingo-systems-20260909/systems-contract.json")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    command = [str(binary), "mcp", "serve", "--toolsets", "all", "--tier", "trusted"]
    requests = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "protocolVersion": "2024-11-05", "capabilities": {},
            "clientInfo": {"name": "lingo-systems-contract", "version": "1"}}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}},
        *[{"jsonrpc": "2.0", "id": i, "method": "tools/call", "params": {
            "name": name, "arguments": {}}} for i, name in enumerate(TOOLS, start=3)],
    ]
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "DSCO_ENV_FILE": "/dev/null",
           "DSCO_PRICING_OFFLINE": "1", "DSCO_SECURE_STORE_NO_PROMPT": "1",
           "DSCO_GOV_MODEL": "standard", "DSCO_NO_AUTO_SUPERVISE": "1",
           "DSCO_TOOLMGMT": "0", "DSCO_NO_COLOR": "1"}
    with tempfile.TemporaryDirectory(prefix="dsco-systems-contract-") as work:
        process = subprocess.run(command, input="".join(json.dumps(r) + "\n" for r in requests),
                                 cwd=work, env=env, capture_output=True, text=True, timeout=30)
    responses = {}
    for line in process.stdout.splitlines():
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict) and "id" in value:
            responses[value["id"]] = value
    init = responses.get(1, {}).get("result", {})
    advertised = {t["name"]: t for t in responses.get(2, {}).get("result", {}).get("tools", [])
                  if t.get("name") in TOOLS}
    report = {"checked_at": datetime.now(timezone.utc).isoformat(), "command": command,
              "binary_sha256": digest(binary), "mcp_process_exit": process.returncode,
              "server_info": init.get("serverInfo"), "mcp_protocol_version": init.get("protocolVersion"),
              "validator": "jsonschema " + version("jsonschema") + " / Draft 2020-12",
              "scope": "Actual final-binary MCP input advertisement, native error calls, and recorded actual-service output envelopes; no backend calls during this check",
              "tools": {}, "checks": [], "limitations": [
                  "This MCP server's tools/list advertises inputSchema only; registered native output schemas below are source contracts, not wire outputSchema advertisement.",
                  "MCP tool results use text content, not structuredContent. Invalid-input calls below exercise the real text-envelope path.",
                  "JSON Schema character lengths do not enforce native UTF-8 byte limits or aggregate response/input byte caps.",
                  "Duplicate object keys and data-dependent identity/count/budget relationships require native validation; JSON Schema checks here do not replace runtime adversarial tests.",
                  "Chimera's registered decision schema is an object envelope; native validation enforces role/stage/model consistency and the supplied execution budgets.",
              ]}

    def check(name, passed, detail=None):
        row = {"name": name, "status": "PASS" if passed else "FAIL"}
        if detail is not None:
            row["detail"] = detail
        report["checks"].append(row)

    def validate(name, schema, value, expected=True):
        errors = list(Draft202012Validator(schema).iter_errors(value))
        detail = {"expected_valid": expected, "observed_valid": not errors}
        if errors:
            first = errors[0]
            # Do not serialize unbounded rejected values into the evidence.
            detail["first_error"] = {"instance_path": list(first.absolute_path),
                                     "schema_path": list(first.absolute_schema_path),
                                     "validator": first.validator}
        check(name, (not errors) == expected, detail)

    check("MCP metadata process exits cleanly", process.returncode == 0)
    check("both systems tools are advertised", set(advertised) == set(TOOLS))
    inputs, outputs = {}, {}
    for name, prefix in TOOLS.items():
        source = ROOT / "src" / (prefix + ".c")
        inp = source_schema(source, prefix + "_schema")
        out = source_schema(source, prefix + "_output_schema")
        for kind, schema in (("input", inp), ("output", out)):
            Draft202012Validator.check_schema(schema)
            check(name + " " + kind + " is valid JSON Schema", True)
        wire = advertised.get(name, {})
        check(name + " actual MCP input equals current native source schema", wire.get("inputSchema") == inp)
        inputs[name], outputs[name] = wire.get("inputSchema", {}), out
        report["tools"][name] = {"advertised": wire, "native_output_schema": out,
                                 "output_schema_advertised": "outputSchema" in wire,
                                 "source": str(source.relative_to(ROOT)), "source_sha256": digest(source)}

    for label, value, valid in [
        ("minimal", {"query": "uppercase"}, True),
        ("explicit bounded", {"source": "tool_management", "query": "uppercase", "limit": 16}, True),
        ("integral JSON number", {"query": "uppercase", "limit": 4.0}, True),
        ("Unicode text", {"query": "café"}, True),
        ("missing query", {}, False), ("empty query", {"query": ""}, False),
        ("blank query", {"query": "   "}, False),
        ("control query", {"query": "upper\ncase"}, False),
        ("terminal newline query", {"query": "uppercase\n"}, False),
        ("NUL query", {"query": "upper\u0000case"}, False),
        ("DEL query", {"query": "upper\u007fcase"}, False),
        ("query over character cap", {"query": "x" * 513}, False),
        ("wrong source", {"source": "filesystem", "query": "uppercase"}, False),
        ("unknown field", {"query": "uppercase", "url": "https://example.invalid"}, False),
        ("zero limit", {"query": "uppercase", "limit": 0}, False),
        ("excessive limit", {"query": "uppercase", "limit": 17}, False),
        ("fractional limit", {"query": "uppercase", "limit": 1.5}, False),
        ("string limit", {"query": "uppercase", "limit": "4"}, False),
        ("boolean limit", {"query": "uppercase", "limit": True}, False),
    ]:
        validate("autobot_discover input: " + label, inputs["autobot_discover"], value, valid)

    base = {"action": "plan", "policy": {}, "request": {"task": "Choose a route"}}
    cases = [("minimal", base, True)]
    for label, target, field, value, valid in [
        ("bounded weights", "policy", "quality_weight", .7, True),
        ("model allowlist", "policy", "allowed_models", ["fixture/model"], True),
        ("integral context", "policy", "min_context", 1000.0, True),
        ("negative weight", "policy", "cost_weight", -1, False),
        ("unknown preference", "policy", "not_a_preference", 1, False),
        ("allowlist object", "policy", "allowed_models", {}, False),
        ("excessive allowlist", "policy", "allowed_models", ["model"] * 26, False),
        ("empty allowed model", "policy", "allowed_models", [""], False),
        ("boolean policy", "policy", "zdr_required", True, True),
        ("string boolean", "policy", "zdr_required", "true", False),
        ("integral call cap", "request", "max_calls", 1.0, True),
        ("bounded budget", "request", "max_worst_case_cost_usd", .25, True),
        ("empty task", "request", "task", "", False),
        ("task over character cap", "request", "task", "x" * 16385, False),
        ("unknown strategy", "request", "strategy", "execute", False),
        ("zero calls", "request", "max_calls", 0, False),
        ("fractional calls", "request", "max_calls", 1.5, False),
        ("excessive calls", "request", "max_calls", 4, False),
        ("excessive cost", "request", "max_expected_cost_usd", .201, False),
        ("excessive tokens", "request", "max_output_tokens", 4097, False),
        ("unknown plan field", "request", "url", "https://example.invalid", False),
    ]:
        value_record = copy.deepcopy(base)
        value_record[target][field] = value
        cases.append((label, value_record, valid))
    cases.extend([( "missing request", {"action": "plan", "policy": {}}, False),
                  ("wrong action", dict(base, action="execute"), False),
                  ("unknown top field", dict(base, session="persistent"), False)])
    for label, value, valid in cases:
        validate("chimera_route input: " + label, inputs["chimera_route"], value, valid)

    for rpc_id, name in enumerate(TOOLS, start=3):
        result = responses.get(rpc_id, {}).get("result", {})
        content = result.get("content", [])
        text = "".join(part.get("text", "") for part in content if part.get("type") == "text")
        try:
            error = json.loads(text)
        except json.JSONDecodeError:
            error = None
        check(name + " MCP invalid input returns native invalid_request", isinstance(error, dict) and
              error.get("ok") is False and error.get("error", {}).get("code") == "invalid_request")
        check(name + " MCP error is marked isError", result.get("isError") is True)
        validate(name + " actual MCP error matches registered output schema", outputs[name], error)
        report["tools"][name]["mcp_invalid_input_result"] = result

    live = json.loads(args.live_report.read_text())
    native_outputs = live.get("live", {}).get("native_outputs", {})
    report["live_evidence"] = {"path": str(args.live_report), "sha256": digest(args.live_report),
                               "status": live.get("status"), "native_outputs": native_outputs}
    check("actual-services source report passed", live.get("status") == "PASS" and live.get("live", {}).get("status") == "PASS")
    service_report_path = args.live_report.parent / "services-verification.json"
    if service_report_path.exists():
        service_report = json.loads(service_report_path.read_text())
        identity = service_report.get("artifact_identity", {}).get("dsco", {}).get("binary", {})
        report["live_evidence"]["service_identity"] = {
            "path": str(service_report_path), "sha256": digest(service_report_path),
            "executed_dsco": identity,
            "observed_artifact_inputs_unchanged": service_report.get("observed_artifact_inputs_unchanged")}
        check("actual-services launcher completed with stable artifacts", service_report.get("status") == "PASS" and
              service_report.get("observed_artifact_inputs_unchanged") is True)
        check("advertised binary matches actual-services executed binary", identity.get("sha256") == report["binary_sha256"])
    for name in TOOLS:
        observed = native_outputs.get(name)
        validate(name + " actual-service native output matches registered schema", outputs[name], observed)
        if not isinstance(observed, dict):
            continue
        if name == "autobot_discover":
            validate(name + " success is not an empty object", outputs[name], {}, False)
            validate(name + " ok true cannot mark an error", outputs[name], {"ok": True, "error": {}}, False)
            bad = copy.deepcopy(observed)
            bad.pop("showing", None)
            validate(name + " missing required success count", outputs[name], bad, False)
            bad = copy.deepcopy(observed)
            bad["matches"] = [{}]
            validate(name + " incomplete match metadata", outputs[name], bad, False)
        else:
            validate(name + " incomplete success", outputs[name], {"ok": True}, False)
            validate(name + " incomplete error", outputs[name], {"ok": False, "error": {}}, False)
            for field, value in (("executed", True), ("scope", "session")):
                validate(name + " invalid " + field, outputs[name], dict(observed, **{field: value}), False)
            validate(name + " actual normalized input is advertised", inputs[name],
                     {"action": "plan", "policy": observed["policy"], "request": observed["request"]})

    report["passed"] = sum(row["status"] == "PASS" for row in report["checks"])
    report["failed"] = [row for row in report["checks"] if row["status"] == "FAIL"]
    report["status"] = "FAIL" if report["failed"] else "PASS"
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"status": report["status"], "passed": report["passed"],
                      "failed": len(report["failed"]), "report": str(args.report)}))
    return 1 if report["failed"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
