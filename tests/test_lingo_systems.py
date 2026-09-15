#!/usr/bin/env python3
"""Exercise real DSCO/Lingo adapters together; fixtures are not backend proof.

Optional live proof uses three explicitly supplied isolated services and one
known pure Autobot tool accepting {text: string}; it performs routing decisions,
not model inference. Credentials are environment-only, never report contents.
"""
import argparse
from contextlib import contextmanager
import copy
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
from urllib.parse import parse_qs, unquote, urlsplit


ROOT = Path(__file__).resolve().parents[1]
REMOTE_ID = "systems/uppercase"
NODE_ID = "czBuMTA"
IMPORTS = ('local l=require("lingo");local d=require("lingo.dsco");'
           'local a=require("lingo.autobot");local g=require("lingo.graphsub");'
           'local c=require("lingo.chimera");'
           'local function selected(found) for _,m in ipairs(found.matches) do '
           'if m.remote_tool_id=="systems/uppercase" then return found.tools[m.name] end '
           'end error("expected exact Autobot tool missing") end;')
DISCOVER = "a.discover{source='tool_management',query='uppercase',limit=4}"
PLAN = "c.preferences{quality_weight=.7,cost_weight=.2,latency_weight=.1}:plan{task='Select a route',strategy='direct',max_calls=1,max_output_tokens=64}"


def route_fixture():
    """A deliberately labeled transport fixture using the native route shape."""
    return {
        "selected_model": "fixture/model", "fallback_models": [], "reasons": ["fixture"],
        "policy_source": "request", "request_patch": {"model": "fixture/model", "provider": "fixture"},
        "model_identity": "dsco-router/chimera:latest",
        "orchestration": {
            "model": "dsco-router/chimera:latest", "runtime": "transport-fixture",
            "operational_artifact_sha256": "0" * 64, "quality_artifact_sha256": "1" * 64,
            "hypercube_snapshot_id": "2" * 64, "hypercube_manifest_sha256": "3" * 64,
            "hypercube_projection_sha256": "4" * 64,
            "plan_type": "direct", "selected_model": "fixture/model", "worst_case_calls": 1,
            "expected_cost_usd": .001, "worst_case_cost_usd": .001,
            "roles": {"primary": "fixture/model"},
            "stages": [{"id": "primary", "kind": "model_call", "role": "primary", "model": "fixture/model"}],
        },
    }


class Fixture:
    def __init__(self):
        self.mode = "happy"
        self.requests = []
        self.lock = threading.Lock()

    def reset(self, mode="happy"):
        with self.lock:
            self.mode = mode
            self.requests.clear()

    def calls(self):
        with self.lock:
            return copy.deepcopy(self.requests)


@contextmanager
def transport_fixture():
    fixture = Fixture()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def serve(self):
            length = int(self.headers.get("Content-Length", 0))
            raw = self.rfile.read(length) if length else b""
            request = json.loads(raw) if raw else None
            with fixture.lock:
                mode = fixture.mode
                fixture.requests.append({"method": self.command, "path": self.path,
                                         "body": request})
            parsed = urlsplit(self.path)
            status, body = 200, None
            if self.command == "GET" and parsed.path == "/health":
                data = {"service": "graphsub", "status": "healthy", "version": "fixture"}
            elif self.command == "GET" and parsed.path == "/api/v1/dynamic-schemas":
                data = {"schemas": []}
            elif self.command == "GET" and parsed.path == "/api/v1/shards/0/nodes":
                query = parse_qs(parsed.query)
                data = {"nodes": [{"id": NODE_ID, "node_id": 10, "name": "Person", "type": "TypeNode"}],
                        "total": 1, "offset": int(query.get("offset", [0])[0]),
                        "limit": int(query.get("limit", [25])[0])}
            elif self.command == "GET" and parsed.path == "/api/v1/discover/search":
                remote_id = "systems-uppercase" if parse_qs(parsed.query).get("query") == ["collision-two"] else REMOTE_ID
                data = {"results": [{"tool_id": remote_id, "tool": {
                    "name": "uppercase", "description": "Pure deterministic uppercase fixture",
                    "input_schema": {"type": "object", "properties": {"text": {"type": "string"}},
                                     "required": ["text"], "additionalProperties": False},
                    "output_schema": {"type": "object"},
                }}]}
                if mode == "empty_discovery":
                    data = {"results": []}
                elif mode == "malformed_discovery":
                    body = b'{"results":'
                elif mode == "discovery_http_error":
                    status, data = 403, {"error": "fixture permission denied"}
            elif self.command == "POST" and unquote(parsed.path) in (
                    "/api/v1/tools/" + REMOTE_ID + "/execute", "/api/v1/tools/systems-uppercase/execute"):
                remote_id = unquote(parsed.path)[len("/api/v1/tools/"):-len("/execute")]
                data = {"tool_id": remote_id, "tool_name": "uppercase", "status": "success",
                        "output": request["inputs"]["text"].upper(), "error": None}
                if mode == "malformed_execution":
                    body = b"not JSON"
                elif mode == "execution_http_error":
                    status, data = 403, {"error": "fixture permission denied"}
                elif mode == "execution_application_error":
                    data.update(status="error", output=None, error="fixture application failure")
            elif self.command == "POST" and parsed.path == "/v1/route":
                data = route_fixture()
                if mode == "malformed_route":
                    data = {"error": "HTTP 200 is not a route decision"}
                elif mode == "wrong_route_identity":
                    data["model_identity"] = "some-other-router"
                elif mode == "route_http_error":
                    status, data = 403, {"error": "fixture permission denied"}
                elif mode == "route_call_overrun":
                    data["orchestration"]["worst_case_calls"] = 4
                elif mode == "route_cost_overrun":
                    data["orchestration"]["worst_case_cost_usd"] = 99
                elif mode == "route_primary_mismatch":
                    data["orchestration"]["roles"]["primary"] = "fixture/other-model"
                elif mode == "route_stage_mismatch":
                    data["orchestration"]["stages"][0]["model"] = "fixture/other-model"
            else:
                status, data = 404, {"error": "unexpected fixture route"}
            if body is None:
                body = json.dumps(data, separators=(",", ":")).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

        do_GET = serve
        do_POST = serve

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
                        default=ROOT / "reports/lingo-systems-20260909/systems-tests.json")
    parser.add_argument("--live-graphsub-url")
    parser.add_argument("--live-autobot-url")
    parser.add_argument("--live-chimera-url")
    parser.add_argument("--live-autobot-tool")
    parser.add_argument("--live-autobot-query", default="uppercase")
    options = parser.parse_args()
    binary = str(Path(options.binary).resolve(strict=True))
    checks, failures = [], []
    report = {"status": "RUNNING", "binary": binary, "passed": checks, "failed": failures,
              "fixture_scope": "Actual DSCO/Lingo adapters with deterministic HTTP fixtures; no backend-conformance claim",
              "live": {"status": "not_requested"}}

    def check(name, function):
        try:
            function()
        except Exception as exc:
            failures.append({"name": name, "error": str(exc)[:2000]})
            print("FAIL", name, str(exc)[:500], flush=True)
        else:
            checks.append(name)
            print("PASS", name, flush=True)

    with tempfile.TemporaryDirectory(prefix="dsco-lingo-systems-") as directory, transport_fixture() as (fixture, url):
        work = Path(directory)
        source_file = work / "evidence.txt"
        source_file.write_text("systems proof: cafe and distributed state\n")
        sequence = 0
        base_env = {
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "DSCO_ENV_FILE": "/dev/null", "DSCO_PRICING_OFFLINE": "1",
            "DSCO_SECURE_STORE_NO_PROMPT": "1", "DSCO_GOV_MODEL": "standard",
            "DSCO_NO_AUTO_SUPERVISE": "1", "DSCO_TOOLMGMT": "1",
            "GRAPHSUB_HOST": url, "TOOLS_API_URL": url, "CHIMERA_HOST": url,
            "TOOLS_API_TOKEN": "systems-fixture-token", "CHIMERA_API_KEY": "systems-fixture-token",
            "NO_PROXY": "*", "DSCO_NO_COLOR": "1",
        }

        def invoke(source, *, arguments=None, changes=None, success=True, is_path=False):
            nonlocal sequence
            sequence += 1
            env = base_env | {"DSCO_BUFFER_DIR": str(work / ("buffers-" + str(sequence))),
                              "DSCO_SURFACE_DIR": str(work / ("surfaces-" + str(sequence)))} | (changes or {})
            command = [binary, "lingo", "run" if is_path else "eval", str(source)]
            if arguments is not None:
                command.append(json.dumps(arguments))
            proc = subprocess.run(command, cwd=work, env=env, capture_output=True, text=True, timeout=25)
            assert (proc.returncode == 0) == success, (proc.returncode, proc.stdout[:2000], proc.stderr[:1000])
            return json.loads(proc.stdout)

        def mixed():
            fixture.reset()
            output = work / "composed.json"
            result = invoke(ROOT / "examples/lingo/cross-system.lingo", is_path=True,
                            arguments={"input": str(source_file), "output": str(output),
                                       "autobot_tool": REMOTE_ID, "autobot_query": "uppercase"})
            value = result["value"]
            saved = json.loads(output.read_text())
            assert saved == value["summary"]
            assert saved["consistency"] == "independent_live_observations"
            assert saved["graphsub"]["page"]["snapshot"] is False
            assert saved["graphsub"]["object_ids"] == [NODE_ID]
            assert saved["autobot"]["result"]["status"] == "success"
            assert saved["autobot"]["result"]["output"] == saved["autobot"]["input"]["text"].upper()
            assert saved["chimera"]["executed"] is False
            assert saved["chimera"]["scope"] == "request"
            assert saved["chimera"]["policy"]["quality_weight"] == .7
            assert result["tool_calls"] == 9 + value["artifact"]["read_pages"], result["tool_calls"]
            requests = fixture.calls()
            assert [x["method"] for x in requests] == ["GET", "GET", "GET", "GET", "POST", "POST"]
            discovery = next(x for x in requests if x["path"].startswith("/api/v1/discover/search"))
            assert parse_qs(urlsplit(discovery["path"]).query) == {"query": ["uppercase"], "limit": ["4"]}
            execution = next(x for x in requests if "/execute" in x["path"])
            assert execution["path"] == "/api/v1/tools/systems%2Fuppercase/execute"
            assert execution["body"] == {"inputs": saved["autobot"]["input"]}
            routing = next(x for x in requests if x["path"] == "/v1/route")
            assert routing["body"]["model"] == "dsco-router/chimera:latest"
            assert routing["body"]["routing_policy"] == saved["chimera"]["policy"]
            assert routing["body"]["orchestration_policy"]["max_calls"] == 1
            assert not any("/chat/completions" in x["path"] for x in requests)
            report["mixed_fixture"] = {"tool_calls": result["tool_calls"], "requests": requests,
                                       "saved_bytes": output.stat().st_size,
                                       "buffer_revision": value["artifact"]["revision"],
                                       "backend_writes": "Only pure uppercase execution; GraphSub GETs and decision-only Router POST"}

        check("mixed workflow preserves observations, exact Autobot inputs and Chimera preferences in real buffer/file bytes", mixed)

        def pure_handles():
            fixture.reset()
            source = (IMPORTS + "local p=c.preferences{allowed_models=l.array{'fixture/model'}};"
                      "local copy=p:inspect();copy.allowed_models[1]='changed';"
                      "local handle=d.tool('read_file');return {name=handle.name,policy=p:inspect(),"
                      "alias=g==require('lingo.operator')}")
            result = invoke(source)
            assert not fixture.calls() and result["tool_calls"] == 0
            assert result["value"]["alias"] is True
            assert result["value"]["name"] == "read_file"
            assert result["value"]["policy"]["allowed_models"] == ["fixture/model"]
            assert result["value"]["policy"]["denied_models"] == []

        check("module imports and immutable request preferences are local with defensive array copies", pure_handles)

        def numeric_boundary():
            fixture.reset()
            source = (IMPORTS + "local fraction=l.decode(l.encode(.25));"
                      "local zero=l.decode(l.encode(-0.0));"
                      "local text=string.rep('π',7000)..'end';"
                      "local b=d.json('buffer',{action='create',name='paged',content=text});"
                      "local parts,offset,pages={},0,0;repeat "
                      "local r=d.json('buffer',{action='read',buffer_id=b.buffer.buffer_id,"
                      "offset=offset,max_bytes=2049});"
                      "parts[#parts+1]=r.text;offset=r.next_offset;pages=pages+1;"
                      "if not r.truncated then break end until false;"
                      "return {fraction=fraction,negative_zero=1/zero==-math.huge,"
                      "equal=table.concat(parts)==text,pages=pages,bytes=offset}")
            value = invoke(source)["value"]
            assert value["fraction"] == .25 and value["negative_zero"] is True, value
            assert value["equal"] is True and value["pages"] > 1 and value["bytes"] == 14003, value
            assert not fixture.calls()

        check("whole-valued tool arguments page UTF-8 buffers while fractional and negative-zero values survive JSON", numeric_boundary)

        def restricted_profile():
            fixture.reset()
            invoke("return 42", changes={"DSCO_TOOL_PROFILE": "restricted"}, success=False)
            assert not fixture.calls()

        check("scripting initialization does not broaden the restricted tool profile", restricted_profile)

        def blocked_network(operation):
            fixture.reset()
            result = invoke(IMPORTS + "return " + operation,
                            changes={"DSCO_ALLOW_NET": "0"}, success=False)
            assert not fixture.calls(), fixture.calls()
            assert "net" in json.dumps(result).lower() or "capability" in json.dumps(result).lower(), result

        for name, operation in [("GraphSub", "g.attach{}"), ("Autobot", DISCOVER), ("Chimera", PLAN)]:
            check(name + " cannot bypass NET=0", lambda op=operation: blocked_network(op))

        def excluded_destination(operation):
            fixture.reset()
            invoke(IMPORTS + "return " + operation,
                   changes={"DSCO_ALLOW_NET": "allowed.invalid", "DSCO_ALLOW_RUN": "1"},
                   success=False)
            assert not fixture.calls(), fixture.calls()

        for name, operation in [("GraphSub", "g.attach{}:status()"),
                                ("Autobot", "(function()local f=" + DISCOVER + ";return #f.matches end)()"),
                                ("Chimera", PLAN)]:
            check(name + " checks the configured destination against scoped NET grants before HTTP",
                  lambda op=operation: excluded_destination(op))

        def permitted_destination():
            fixture.reset()
            source = (IMPORTS + "local found=" + DISCOVER + ";"
                      "local transformed=selected(found):json{text='allowed host'};"
                      "local graph=g.attach{}:status();local plan=" + PLAN + ";"
                      "return {autobot=transformed.status,output=transformed.output,"
                      "graphsub=graph.ok,chimera=plan.ok,executed=plan.executed}")
            value = invoke(source, changes={"DSCO_ALLOW_NET": "127.0.0.1", "DSCO_ALLOW_RUN": "1"})["value"]
            assert value == {"autobot": "success", "output": "ALLOWED HOST", "graphsub": True,
                             "chimera": True, "executed": False}, value
            assert len(fixture.calls()) == 5, fixture.calls()
            assert sum(x["method"] == "POST" for x in fixture.calls()) == 2

        check("allowed loopback NET scope permits GraphSub, Autobot discovery and leaf execution, and Chimera", permitted_destination)

        def blocked_write():
            fixture.reset()
            value = invoke(IMPORTS + 'return d.tool("buffer"):call{action="create",name="blocked",content="no"}',
                           changes={"DSCO_ALLOW_WRITE": "0"})["value"]
            assert value["ok"] is False, value
            assert not fixture.calls()

        check("DSCO typed handles retain WRITE=0 denial", blocked_write)

        def shared_taint():
            fixture.reset()
            source = (IMPORTS + "local found=" + DISCOVER + ";"
                      "d.json('buffer',{action='create',name='synthetic-sensitive',"
                      "content='synthetic only',sensitive=true});"
                      "return selected(found):call{text='must stay local'}")
            value = invoke(source, changes={"DSCO_ALLOW_SECRETS": "1"})["value"]
            assert value["ok"] is False, value
            assert any(word in value["result"].lower() for word in ("exfil", "trifecta", "untrusted")), value
            assert len(fixture.calls()) == 1 and fixture.calls()[0]["path"].startswith("/api/v1/discover/search"), fixture.calls()

        check("Autobot leaf execution preserves prior untrusted and sensitive DSCO buffer taint", shared_taint)

        def pure_boundary(operation, scenario):
            fixture.reset()
            if scenario:
                source = IMPORTS + "local w=l.world();return w:scenario('s',function()return " + operation + " end)"
            else:
                source = IMPORTS + "local w=l.world();w:define('Probe',{value=l.derived('json',function()return " + operation + " end)});return w:new('Probe','p',{}).value"
            result = invoke(source, success=False)
            assert not fixture.calls(), fixture.calls()
            assert ("scenario" if scenario else "calculated") in json.dumps(result).lower(), result

        pure_operations = [("DSCO", 'd.tool("read_file"):run{path=args.path}'),
                           ("Autobot", DISCOVER), ("GraphSub", "g.attach{}"), ("Chimera", PLAN)]
        for name, operation in pure_operations:
            for scenario in (False, True):
                check(name + (" scenario" if scenario else " calculation") + " cannot execute host interactions",
                      lambda op=operation, sc=scenario: pure_boundary(op, sc))

        def remote_failure(mode, operation):
            fixture.reset(mode)
            invoke(IMPORTS + "return " + operation, success=False)
            assert fixture.calls(), "failure happened without exercising transport"
            if mode in ("malformed_execution", "execution_http_error", "execution_application_error"):
                assert fixture.calls()[-1]["method"] == "POST" and fixture.calls()[-1]["path"].endswith("/execute"), fixture.calls()

        execution = "(function()local found=" + DISCOVER + ";return selected(found):json{text='hello'}end)()"
        for mode, operation in [("malformed_discovery", DISCOVER), ("discovery_http_error", DISCOVER),
                                ("malformed_execution", execution), ("execution_http_error", execution),
                                ("execution_application_error", execution),
                                ("malformed_route", PLAN), ("wrong_route_identity", PLAN), ("route_http_error", PLAN),
                                ("route_call_overrun", PLAN), ("route_cost_overrun", PLAN),
                                ("route_primary_mismatch", PLAN), ("route_stage_mismatch", PLAN)]:
            check(mode + " is not accepted as successful typed output", lambda m=mode, op=operation: remote_failure(m, op))

        def empty_discovery():
            fixture.reset("empty_discovery")
            result = invoke(IMPORTS + "local f=" + DISCOVER + ";return {matches=f.matches,none=next(f.tools)==nil}")["value"]
            assert result == {"matches": [], "none": True}, result

        check("genuine empty discovery is distinct from transport failure", empty_discovery)

        def retained_handle_identity():
            fixture.reset()
            source = (IMPORTS + "local first=" + DISCOVER + ";local captured=selected(first);"
                      "local second=a.discover{query='collision-two',limit=4};"
                      "local other;for _,m in ipairs(second.matches) do "
                      "if m.remote_tool_id=='systems-uppercase' then other=m.name end end;"
                      "assert(other,'second exact tool missing');"
                      "local value=captured:json{text='original route'};"
                      "return {first=captured:describe().name,second=other,value=value}")
            value = invoke(source)["value"]
            assert value["first"] != value["second"], value
            assert value["value"]["tool_id"] == REMOTE_ID, value
            assert fixture.calls()[-1]["path"] == "/api/v1/tools/systems%2Fuppercase/execute", fixture.calls()

        check("later colliding discovery cannot retarget a captured Autobot tool handle", retained_handle_identity)

        for name, source in [
            ("Autobot unknown option", "a.discover{query='uppercase',surprise=true}"),
            ("Autobot invalid source", "a.discover{source='anything',query='uppercase'}"),
            ("Chimera unknown preference", "c.preferences{surprise=true}"),
            ("Chimera invalid plan budget", "c.preferences{}:plan{task='route',max_calls=0}"),
            ("immutable DSCO handle", "(function()local t=d.tool('read_file');t.name='write_file';return t end)()"),
            ("immutable Chimera preferences", "(function()local p=c.preferences{quality_weight=.7};p.quality_weight=0;return p end)()"),
        ]:
            def malformed_request(expression=source):
                fixture.reset()
                invoke(IMPORTS + "return " + expression, success=False)
                assert not fixture.calls(), fixture.calls()
            check(name + " rejected before HTTP", malformed_request)

        live_options = [options.live_graphsub_url, options.live_autobot_url,
                        options.live_chimera_url, options.live_autobot_tool]
        if any(live_options):
            def live():
                assert all(live_options), "live proof requires all three service URLs and the exact pure Autobot tool"
                output = work / "live-composed.json"
                result = invoke(ROOT / "examples/lingo/cross-system.lingo", is_path=True,
                                arguments={"input": str(source_file), "output": str(output),
                                           "autobot_tool": options.live_autobot_tool,
                                           "autobot_query": options.live_autobot_query},
                                changes={"GRAPHSUB_HOST": options.live_graphsub_url,
                                         "TOOLS_API_URL": options.live_autobot_url,
                                         "CHIMERA_HOST": options.live_chimera_url,
                                         "TOOLS_API_TOKEN": os.environ.get("LINGO_SYSTEMS_LIVE_AUTOBOT_TOKEN", ""),
                                         "CHIMERA_API_KEY": os.environ.get("LINGO_SYSTEMS_LIVE_CHIMERA_TOKEN", "")})
                value = result["value"]
                assert json.loads(output.read_text()) == value["summary"]
                assert value["summary"]["chimera"]["executed"] is False
                assert value["summary"]["graphsub"]["page"]["snapshot"] is False
                assert value["summary"]["autobot"]["result"]["status"] == "success"
                assert value["summary"]["autobot"]["result"]["output"] == value["summary"]["autobot"]["input"]["text"].upper()
                discovery_receipt = value["receipts"]["discovery"]
                assert discovery_receipt["ok"] is True
                discovery_output = json.loads(discovery_receipt["result"])
                assert discovery_output["source"] == "tool_management_api"
                assert discovery_output["query"] == options.live_autobot_query
                artifact = options.report.parent / "cross-system-live.json"
                artifact.parent.mkdir(parents=True, exist_ok=True)
                artifact.write_bytes(output.read_bytes())
                report["live"] = {"status": "PASS", "tool_calls": result["tool_calls"],
                                  "summary": value["summary"], "saved_bytes": output.stat().st_size,
                                  "native_outputs": {"autobot_discover": discovery_output,
                                                     "chimera_route": value["summary"]["chimera"]},
                                  "artifact": str(artifact),
                                  "scope": "Actual supplied GraphSub, Autobot pure text tool and Chimera route endpoints; no model inference or GraphSub writes"}
            check("actual services compose through one governed Lingo workflow", live)

    report["status"] = "FAIL" if failures else "PASS"
    options.report.parent.mkdir(parents=True, exist_ok=True)
    options.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"status": report["status"], "passed": len(checks), "failed": len(failures),
                      "report": str(options.report)}), flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
