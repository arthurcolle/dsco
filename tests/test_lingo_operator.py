#!/usr/bin/env python3
"""Real DSCO/Lingo binding tests; HTTP fixtures prove transport, not GraphSub.

Optional genuine engine read proof:
  LINGO_GRAPHSUB_LIVE_URL=http://127.0.0.1:17879 python3 tests/test_lingo_operator.py ./dsco
No test writes to a GraphSub backend. The fixture deliberately serves hostile data.
"""
import argparse
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import threading
from urllib.parse import parse_qs, urlsplit


ROOT = Path(__file__).resolve().parents[1]
RESPONSE_LIMIT = 128 * 1024
PERSON_ID = "czBuMTA"
IMPORT = 'local l=require("lingo");local o=require("lingo.operator");'
ATTACH = IMPORT + 'local s=o.attach{connection="default",shard_id=0};'
PROXY_KEYS = {"http_proxy", "https_proxy", "all_proxy", "no_proxy"}


class Fixture:
    def __init__(self):
        self.mode = "happy"
        self.requests = []
        self.lock = threading.Lock()

    def reset(self, mode="happy"):
        with self.lock:
            self.mode = mode
            self.requests.clear()

    def paths(self):
        with self.lock:
            return list(self.requests)


@contextmanager
def transport_fixture():
    fixture = Fixture()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            with fixture.lock:
                fixture.requests.append((self.command, self.path))
                mode = fixture.mode
            status, headers = 200, {}
            if mode == "malformed":
                body = b'{"broken":'
            elif mode == "trailing":
                body = b'{"status":"healthy"} trailing'
            elif mode == "duplicate":
                body = b'{"service":"graphsub","status":"healthy","status":"healthy","version":"fixture"}'
            elif mode == "unsafe_integer":
                body = b'{"service":"graphsub","status":"healthy","version":"fixture","node_id":9007199254740993}'
            elif mode == "error_envelope":
                body = b'{"error":"application failure despite HTTP 200"}'
            elif mode == "not_found":
                status, body = 404, b'{"code":404,"error":"Node not found"}'
            elif mode == "redirect":
                status, body = 302, b'{"message":"redirect must not be followed"}'
                headers["Location"] = "/redirect-target"
            elif mode in ("oversized", "at_cap"):
                # A valid health response at or exactly one byte over the cap.
                base = b'{"service":"graphsub","status":"healthy","version":"fixture","padding":""}'
                length = RESPONSE_LIMIT + (mode == "oversized")
                body = base[:-2] + b"x" * (length - len(base)) + base[-2:]
                assert len(body) == length
            else:
                parsed = urlsplit(self.path)
                if parsed.path == "/health":
                    data = {"service": "graphsub", "status": "healthy", "version": "fixture"}
                elif parsed.path == "/api/v1/dynamic-schemas":
                    data = {"schemas": {} if mode == "bad_schema" else []}
                elif parsed.path == "/api/v1/shards/0/nodes":
                    query = parse_qs(parsed.query)
                    data = {"nodes": [{"id": PERSON_ID, "name": "Person", "node_id": 10,
                              "shard_id": 0, "type": "TypeNode", "type_id": 0,
                              "confidence": 1.0, "epistemic_status": "Assumed"}],
                            "limit": int(query.get("limit", [25])[0]),
                            "offset": int(query.get("offset", [0])[0]), "total": 243}
                    if mode == "bad_list_echo":
                        data["offset"] += 1
                    elif mode in ("clamped_page", "empty_graph"):
                        data["total"] = 0 if mode == "empty_graph" else 243
                        data["offset"] = min(data["offset"], data["total"])
                        data["nodes"] = []
                    elif mode == "unnamed":
                        data["nodes"][0]["name"] = ""
                        data["nodes"][0]["type"] = ""
                elif parsed.path in ("/api/v1/shards/0/nodes/" + PERSON_ID,
                                     "/api/v1/shards/0/nodes/10"):
                    data = {"id": PERSON_ID, "name": "Person", "node_id": 10,
                            "type": "TypeNode", "payload": None,
                            "metadata": {"confidence": 1.0, "epistemic_status": "Assumed"}}
                    if mode in ("bad_read_identity", "bad_read_numeric"):
                        data["id"] = "czBuMTE"
                        data["node_id"] = 11
                    elif mode == "unnamed":
                        data["name"] = ""
                        data["type"] = ""
                else:
                    status, data = 404, {"error": "Unexpected fixture route"}
                body = json.dumps(data, separators=(",", ":")).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            for name, value in headers.items():
                self.send_header(name, value)
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass  # Expected when the client stops at its response byte cap.

        def do_POST(self):
            with fixture.lock:
                fixture.requests.append((self.command, self.path))
            self.send_error(405)

        do_PUT = do_DELETE = do_PATCH = do_POST

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield fixture, "http://127.0.0.1:" + str(server.server_port)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", default=str(ROOT / "dsco"))
    parser.add_argument("--report", type=Path,
                        default=ROOT / "reports/lingo-operator-20260909/operator-tests.json")
    options = parser.parse_args()
    binary = str(Path(options.binary).resolve())
    # Do not inherit a real GraphSub credential or unrelated trust/proxy settings.
    env_base = {key: value for key, value in os.environ.items()
                if not key.startswith(("DSCO_ALLOW_", "DSCO_GOV_", "DSCO_TRUST_",
                                       "DSCO_EXECUTION_", "GRAPHSUB_"))
                and key.lower() not in PROXY_KEYS}
    env_base.update(DSCO_GOV_MODEL="standard", DSCO_NO_COLOR="1", NO_PROXY="*")
    checks = []
    report = {"status": "RUNNING", "binary": binary, "passed": checks, "failed": [],
              "fixture_scope": "Adversarial HTTP transport through the real DSCO Lingo binary; not backend conformance",
              "live_backend": {"status": "not_requested"}}

    def invoke(source, host, *, args=None, env=None, success=True):
        command = [binary, "lingo", "eval", source]
        if args is not None:
            command.append(json.dumps(args))
        proc = subprocess.run(command, cwd=ROOT,
                              env=env_base | {"GRAPHSUB_HOST": host} | (env or {}),
                              capture_output=True, text=True, timeout=20)
        assert (proc.returncode == 0) == success, (proc.returncode, proc.stdout, proc.stderr)
        try:
            return json.loads(proc.stdout)
        except json.JSONDecodeError as exc:
            raise AssertionError((proc.stdout, proc.stderr)) from exc

    def check(name, fn):
        try:
            fn()
        except Exception as exc:
            report["failed"].append({"name": name, "error": str(exc)})
            print("FAIL", name, str(exc), flush=True)
        else:
            checks.append(name)
            print("PASS", name, flush=True)

    def failure(source, host, contains=None, **kwargs):
        response = invoke(source, host, success=False, **kwargs)
        assert isinstance(response.get("error"), str), response
        if contains:
            assert contains.lower() in response["error"].lower(), response
        return response

    try:
        with transport_fixture() as (fixture, host):
            def happy():
                fixture.reset()
                response = invoke(ATTACH + '''
local status=s:status();local schema=s:schema()
local listed=s:list{limit=3,offset=2};local node=s:read("czBuMTA")
return {status=status,schema=schema,listed=listed,node=node}
''', host)
                assert response["tool_calls"] == 5, response
                for label, action in (("status", "status"), ("schema", "schema"),
                                      ("listed", "list"), ("node", "read")):
                    result = response["value"][label]
                    assert result["ok"] is True and result["snapshot"] is False, result
                    assert result["profile"] == "browse" and result["consistency"] == "live", result
                    assert result["connection"] == "default" and result["shard_id"] == 0, result
                    assert result["action"] == action and result["observed_at"], result
                assert response["value"]["node"]["data"]["id"] == PERSON_ID
                assert response["value"]["node"]["data"]["payload"] is None
                paths = fixture.paths()
                assert paths[:3] == [("GET", "/health"), ("GET", "/health"),
                                     ("GET", "/api/v1/dynamic-schemas")], paths
                assert paths[4] == ("GET", "/api/v1/shards/0/nodes/" + PERSON_ID), paths
                parsed = urlsplit(paths[3][1])
                assert paths[3][0] == "GET" and parsed.path == "/api/v1/shards/0/nodes", paths
                assert parse_qs(parsed.query) == {"limit": ["3"], "offset": ["2"]}, paths
            check("fixture browse uses only the four allowed GET routes with live provenance", happy)

            def defaults():
                fixture.reset()
                result = invoke(IMPORT + 'local s=o.attach{shard_id=0.0};return s:list{offset=0.0}', host)
                assert result["value"]["data"]["limit"] == 25
                assert result["value"]["data"]["offset"] == 0
                assert result["tool_calls"] == 2
            check("attach and list defaults remain explicit and bounded", defaults)

            def numeric_string_identity():
                fixture.reset()
                result = invoke(ATTACH + 'return s:read("10")', host)
                assert result["value"]["data"]["id"] == PERSON_ID, result
                assert fixture.paths() == [("GET", "/health"),
                                           ("GET", "/api/v1/shards/0/nodes/10")], fixture.paths()
            check("backend-compatible numeric string ID stays a literal path component", numeric_string_identity)

            def clamped_pages():
                # Native rest_list_nodes echoes min(request.offset, total).
                for mode, total in (("clamped_page", 243), ("empty_graph", 0)):
                    fixture.reset(mode)
                    result = invoke(ATTACH + 'return s:list{limit=1,offset=10000000}', host)
                    data = result["value"]["data"]
                    assert data["nodes"] == [] and data["offset"] == total and data["total"] == total, data
            check("native beyond-end and empty-graph pages accept the clamped offset", clamped_pages)

            def unnamed_nodes():
                fixture.reset("unnamed")
                result = invoke(ATTACH + 'return {page=s:list(),node=s:read("czBuMTA")}', host)
                for node in (result["value"]["page"]["data"]["nodes"][0], result["value"]["node"]["data"]):
                    assert node["name"] == "" and node["type"] == "", node
            check("native unnamed node and type labels remain valid strings", unnamed_nodes)

            invalid_attach = [
                ('o.attach{connection="other"}', "connection alias"),
                ('o.attach{connection=1}', "connection type"),
                ('o.attach{url="http://example.invalid"}', "URL override"),
                ('o.attach{snapshot="claimed"}', "snapshot claim"),
                ('o.attach{shard_id="0"}', "string shard"),
                ('o.attach{shard_id=-1}', "negative shard"),
                ('o.attach{shard_id=0.5}', "fractional shard"),
                ('o.attach{shard_id=2147483648}', "overflow shard"),
                ('o.attach(false)', "non-record attach"),
                ('o.attach({},1)', "extra attach argument"),
            ]
            for expression, label in invalid_attach:
                def reject_attach(expression=expression):
                    fixture.reset()
                    failure(IMPORT + 'return ' + expression, host)
                    assert not fixture.paths(), fixture.paths()
                check("reject " + label + " before HTTP", reject_attach)

            invalid_methods = [
                ('s:status(1)', "status arguments"), ('s:schema({})', "schema arguments"),
                ('s:list{limit=0}', "zero limit"), ('s:list{limit=101}', "oversized limit"),
                ('s:list{limit="2"}', "string limit"), ('s:list{limit=1.5}', "fractional limit"),
                ('s:list{offset=-1}', "negative offset"),
                ('s:list{offset=10000001}', "oversized offset"),
                ('s:list{offset=true}', "boolean offset"),
                ('s:list{type="Person"}', "unimplemented type filter"),
                ('s:list{query="MATCH (n) DELETE n"}', "query injection"),
                ('s:list(false)', "non-record list"), ('s:list({},1)', "extra list argument"),
                ('s:read(10)', "numeric ID coercion"), ('s:read("")', "empty ID"),
                ('s:read(string.rep("a",257))', "oversized ID"),
                ('s:read("a"..string.char(0).."b")', "NUL ID"),
                ('s:read("czBuMTA",1)', "extra read argument"),
            ]
            for expression, label in invalid_methods:
                def reject_method(expression=expression):
                    fixture.reset()
                    failure(ATTACH + 'return ' + expression, host)
                    assert fixture.paths() == [("GET", "/health")], fixture.paths()
                check("reject " + label + " before an operation request", reject_method)

            # Native validation must also hold when callers bypass the Lua facade.
            native_invalid = [
                {"action": "write"}, {"action": "status", "url": "http://example.invalid"},
                {"action": "status", "connection": "other"},
                {"action": "status", "shard_id": "0"},
                {"action": "read", "node_id": 10},
                {"action": "list", "limit": 101}, {"action": "list", "offset": -1},
                {"action": "list", "query": "MATCH (n) DELETE n"},
            ]
            def strict_native():
                fixture.reset()
                result = invoke(IMPORT + '''
for index,arguments in ipairs(args.invalid) do
  local receipt=l.call("graphsub_operator",arguments)
  assert(not receipt.ok,"invalid argument case "..index..": "..receipt.result)
end
return true
''', host, args={"invalid": native_invalid})
                assert result["value"] is True and not fixture.paths(), (result, fixture.paths())
            check("native tool rejects invalid actions fields and typed inputs without HTTP", strict_native)

            for node_id in ("..", "../stats", "czBuMTA/../../stats", "%2e%2e%2fstats",
                            "czBuMTA?limit=999", "czBuMTA#fragment", "a\nb"):
                def path_confinement(node_id=node_id):
                    fixture.reset()
                    failure(ATTACH + 'return s:read(args.id)', host, "invalid_request", args={"id": node_id})
                    paths = fixture.paths()
                    assert paths == [("GET", "/health")], paths
                check("opaque ID cannot traverse or alter route: " + repr(node_id), path_confinement)

            for mode, label, code in (
                    ("malformed", "malformed JSON", "malformed_response"),
                    ("trailing", "JSON trailing data", "malformed_response"),
                    ("duplicate", "duplicate JSON keys", "malformed_response"),
                    ("error_envelope", "HTTP 200 application error envelope", "malformed_response"),
                    ("unsafe_integer", "JSON integer outside Lua exact range", "precision_unsupported"),
                    ("not_found", "HTTP 404", "http_error"),
                    ("redirect", "HTTP redirect", "http_error"),
                    ("oversized", "response cap plus one byte", "response_too_large")):
                def adversarial(mode=mode, code=code):
                    fixture.reset(mode)
                    failure(IMPORT + 'return o.attach{}', host, code)
                    assert fixture.paths() == [("GET", "/health")], fixture.paths()
                check("reject " + label + " without follow-up requests", adversarial)

            def exact_response_cap():
                fixture.reset("at_cap")
                result = invoke(ATTACH + 'return s:status().data.status', host)
                assert result["value"] == "healthy", result
                assert fixture.paths() == [("GET", "/health")] * 2, fixture.paths()
            check("valid response exactly at the byte cap remains readable", exact_response_cap)

            def outer_json_preserved():
                fixture.reset("at_cap")
                result = invoke(ATTACH + 'return s:status().data', host)
                assert result["value"]["status"] == "healthy", result
                assert len(result["value"]["padding"]) > 32768
                assert len(json.dumps(result["value"], separators=(",", ":"))) == RESPONSE_LIMIT
                assert fixture.paths() == [("GET", "/health")] * 2, fixture.paths()
            check("large bounded operator JSON survives the outer Lingo tool result intact", outer_json_preserved)

            for mode, expression in (("bad_schema", "s:schema()"),
                                     ("bad_list_echo", "s:list{limit=1,offset=0}"),
                                     ("bad_read_identity", 's:read("czBuMTA")'),
                                     ("bad_read_numeric", 's:read("10")')):
                def bad_envelope(mode=mode, expression=expression):
                    fixture.reset(mode)
                    failure(ATTACH + "return " + expression, host, "malformed_response")
                    assert len(fixture.paths()) == 2, fixture.paths()
                check("reject HTTP 200 response mismatch: " + mode, bad_envelope)

            def net_denial():
                fixture.reset()
                failure(IMPORT + 'return o.attach{}', host, "DSCO_ALLOW_NET", env={"DSCO_ALLOW_NET": "0"})
                assert not fixture.paths(), fixture.paths()
            check("network capability opt-out denies attach before HTTP", net_denial)

            def calculation_denial():
                fixture.reset()
                failure(ATTACH + '''
local w=l.world();w:define("Remote",{value=l.derived("integer",function()
 s:status();return 1
end)});local x=w:new("Remote","r",{});return x.value
''', host, "calculated value")
                assert fixture.paths() == [("GET", "/health")], fixture.paths()
            check("live I/O cannot occur inside derived value evaluation", calculation_denial)

            def scenario_denial():
                fixture.reset()
                failure(ATTACH + 'return l.world():scenario("hypothetical",function() return s:status() end)',
                        host, "inside a scenario")
                assert fixture.paths() == [("GET", "/health")], fixture.paths()
            check("live I/O cannot occur inside a scenario", scenario_denial)

            def taint_denial():
                fixture.reset()
                result = invoke(ATTACH + '''
-- Only a synthetic classifier marker is printed; no secret file is read.
local marker=l.call("bash",{command='printf "%s\\n" "~/.ssh/id_rsa synthetic-secret-marker"'})
assert(marker.ok,marker.result)
local ok,err=l.try(function() return s:read("czBuMTA") end)
assert(not ok and tostring(err):find("lethal-trifecta",1,true),tostring(err))
return true
''', host)
                assert result["value"] is True and fixture.paths() == [("GET", "/health")], (result, fixture.paths())
            check("GraphSub untrusted input and synthetic secret taint persist across nested calls", taint_denial)

            def immutable():
                fixture.reset()
                result = invoke(ATTACH + '''
local ok,err=l.try(function() s.shard_id=9 end)
assert(not ok and tostring(err):find("immutable",1,true))
assert(not l.try(function() o.attach=function() end end))
assert(not l.try(function() return s:commit{} end))
local first=s:status();first.shard_id=9;first.data.status="tampered"
local second=s:status()
assert(second.shard_id==0 and second.data.status=="healthy")
return true
''', host)
                assert result["value"] is True
                assert fixture.paths() == [("GET", "/health")] * 3, fixture.paths()
            check("scope and API are immutable; returned data cannot rebind later operations", immutable)

            for suffix in ("/prefix", "?token=synthetic", "#fragment", "/../health"):
                def origin_only(suffix=suffix):
                    fixture.reset()
                    failure(IMPORT + 'return o.attach{}', host + suffix)
                    assert not fixture.paths(), fixture.paths()
                check("host configuration rejects non-origin URL " + suffix, origin_only)

        live_url = os.environ.get("LINGO_GRAPHSUB_LIVE_URL")
        if live_url:
            def live_browse():
                result = invoke(ATTACH + '''
local status=s:status();local schema=s:schema()
local listed=s:list{limit=3,offset=0};local person=s:read(args.person_id)
local past_end=s:list{limit=1,offset=10000000}
assert(status.data.service=="graphsub")
assert(person.data.id==args.person_id and person.data.name=="Person")
assert(person.snapshot==false and person.consistency=="live")
assert(#past_end.data.nodes==0 and past_end.data.offset==past_end.data.total)
return {status=status.data,schema=schema.data,list_count=#listed.data.nodes,
 person=person.data,past_end=past_end.data,profile=person.profile,consistency=person.consistency,snapshot=person.snapshot}
''', live_url, args={"person_id": os.environ.get("LINGO_GRAPHSUB_LIVE_PERSON_ID", PERSON_ID)})
                assert result["tool_calls"] == 6 and result["value"]["list_count"] > 0, result
                report["live_backend"] = {"status": "PASS", "tool_calls": result["tool_calls"],
                                           "value": result["value"], "scope": "Read-only live browsing; no snapshot or transaction claim"}
            check("optional genuine GraphSub engine status schema list and opaque object read", live_browse)
        if report["failed"]:
            raise AssertionError("Failed checks: " + "; ".join(item["name"] for item in report["failed"]))
        report["status"] = "PASS"
    except Exception as exc:
        report["status"] = "FAIL"
        report["failure"] = str(exc)
        raise
    finally:
        report["passed_count"] = len(checks)
        options.report.parent.mkdir(parents=True, exist_ok=True)
        options.report.write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2), flush=True)


if __name__ == "__main__":
    main()
