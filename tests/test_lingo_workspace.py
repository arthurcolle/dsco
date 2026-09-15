#!/usr/bin/env python3
"""Governed stored-world contract against an adversarial HTTP transport fixture.

This is not GraphSub persistence proof. scripts/verify_lingo_workspace.py runs
the same Lingo workflow against the actual engine and across its restart.
"""
import argparse
import base64
from contextlib import contextmanager
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import threading

ROOT = Path(__file__).resolve().parents[1]
IMPORT = 'local l=require("lingo");local ws=require("lingo.workspace");'
DECL = '''
local function declared(id,version)
 local w=l.world{id=id or "workspace-test"}
 w:define("Counter",{n=l.stored("integer"),label=l.stored("string",""),
  doubled=l.derived("integer",function(self)return self.n*2 end)}, {version=version or "1"})
 return w
end
local w=declared()
'''
PUBLISH = IMPORT + DECL + 'w:new("Counter","one",{n=21});local a,r=ws.publish(w);return {address=a:inspect(),receipt=r}'


def clean_environment():
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("DSCO_ALLOW_", "DSCO_GOV_", "DSCO_TRUST_", "DSCO_EXECUTION_", "GRAPHSUB_"))
           and k.lower() not in {"http_proxy", "https_proxy", "all_proxy", "no_proxy"}}
    env.update(DSCO_GOV_MODEL="standard", DSCO_NO_COLOR="1", NO_PROXY="*")
    return env


def invoke(binary, source, host, *, args=None, env=None, success=True, path=False):
    command = [str(binary), "lingo", "run" if path else "eval", str(source)]
    if args is not None:
        command.append(json.dumps(args))
    p = subprocess.run(command, cwd=ROOT, env=clean_environment() | {"GRAPHSUB_HOST": host} | (env or {}),
                       capture_output=True, text=True, timeout=30)
    assert (p.returncode == 0) == success, (p.returncode, p.stdout, p.stderr)
    return json.loads(p.stdout)


class Fixture:
    def __init__(self):
        self.mode = "happy"
        self.requests = []
        self.nodes = {}
        self.commits = 10

    def reset(self, mode="happy"):
        self.mode = mode
        self.requests.clear()

    def persistence(self):
        return {"shard_id": 0, "persistent_request_path_enabled": self.mode != "no_persistence",
                "primary_manifest_integrity_mode": None if self.mode == "no_manifest" else "wal_backed_metadata",
                "latest_committed_tx_id": self.commits, "latest_checkpoint_tx_id": 2,
                "wal": {"segment_count": 1, "total_bytes": 1000 + self.commits},
                "timings": {"commit_count": self.commits}}


@contextmanager
def transport_fixture():
    f = Fixture()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def send(self, status, data, headers=None):
            body = data if isinstance(data, bytes) else json.dumps(data, separators=(",", ":")).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            for k, v in (headers or {}).items():
                self.send_header(k, v)
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def do_GET(self):
            f.requests.append((self.command, self.path))
            if f.mode == "malformed":
                return self.send(200, b'{"bad":')
            if f.mode == "duplicate":
                return self.send(200, b'{"x":1,"x":2}')
            if f.mode == "precision":
                return self.send(200, b'{"x":9007199254740993}')
            if f.mode == "oversized":
                return self.send(200, b'{"x":"' + b'x' * (128 * 1024 - 7) + b'"}')
            if f.mode == "not_found":
                return self.send(404, {"error": "not found"})
            if f.mode == "redirect":
                return self.send(302, {}, {"Location": "/must-not-follow"})
            if self.path == "/api/v1/shards/0/persistence":
                return self.send(200, f.persistence())
            key = self.path.removeprefix("/api/v1/shards/0/nodes/")
            if key not in f.nodes:
                return self.send(404, {"error": "not found"})
            data = dict(f.nodes[key])
            if f.mode == "tampered":
                data["payload"] += " "
            if f.mode == "wrong_identity":
                data["node_id"] += 1
            if f.mode == "wrong_type":
                data["type"] = "Blob"
            return self.send(200, data)

        def do_POST(self):
            f.requests.append((self.command, self.path))
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            assert self.path == "/api/v1/shards/0/nodes", self.path
            assert set(body) == {"type_name", "name", "payload"} and body["type_name"] == "State", body
            assert body["name"] == "lingo.world/" + hashlib.sha256(body["payload"].encode()).hexdigest()
            number = len(f.nodes) + 300
            key = base64.urlsafe_b64encode(f"s0n{number}".encode()).decode().rstrip("=")
            data = {"id": key, "node_id": number, "name": body["name"], "type": "State"}
            f.nodes[key] = data | {"payload": body["payload"], "metadata": {}}
            if f.mode != "no_commit_progress":
                f.commits += 1
            if f.mode == "lost_ack":
                return self.send(503, {"error": "ack lost after write"})
            if f.mode == "bad_ack":
                return self.send(200, data)
            return self.send(201, data)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f, f"http://127.0.0.1:{server.server_port}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", default=str(ROOT / "dsco"))
    parser.add_argument("--report", type=Path, default=ROOT / "reports/lingo-world-20260910/transport-tests.json")
    opt = parser.parse_args()
    binary = Path(opt.binary).resolve()
    report = {"status": "RUNNING", "binary": str(binary), "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "scope": "Actual Lingo host; adversarial transport only, not GraphSub conformance", "passed": [], "failed": []}
    with transport_fixture() as (f, host):
        def check(name, fn):
            try:
                fn()
                report["passed"].append(name)
                print("PASS", name)
            except Exception as exc:
                report["failed"].append({"name": name, "error": str(exc)})
                print("FAIL", name, str(exc))

        def run(source, **kwargs):
            return invoke(binary, source, host, **kwargs)

        def happy():
            f.reset()
            result = run(IMPORT + DECL + '''
w:new("Counter","one",{n=21})
local a,r=ws.publish(w)
assert(not l.try(function()a.node_id="overwrite" end))
local copied=a:inspect();copied.world_id="overwritten";assert(a.world_id=="workspace-test")
local loaded=declared();local reopened,receipt=ws.open(a,loaded)
assert(reopened==loaded and loaded:ref("one").doubled==42)
assert(not l.try(function()ws.open(a,loaded)end))
return {address=a:inspect(),snapshot=r.snapshot,receipt=receipt}
''')["value"]
            a = result["address"]
            assert hashlib.sha256(result["snapshot"].encode()).hexdigest() == a["sha256"]
            assert f.requests == [("GET", "/api/v1/shards/0/persistence"), ("POST", "/api/v1/shards/0/nodes"),
                                  ("GET", "/api/v1/shards/0/nodes/" + a["node_id"]), ("GET", "/api/v1/shards/0/persistence"),
                                  ("GET", "/api/v1/shards/0/persistence"), ("GET", "/api/v1/shards/0/nodes/" + a["node_id"]),
                                  ("GET", "/api/v1/shards/0/persistence")], f.requests
            assert result["receipt"]["persistence"] == "server_reported"
        check("publish and reopen via governed native paths; immutable address and cached recomputation", happy)

        def versions():
            f.reset()
            r = run(IMPORT + DECL + '''
local x=w:new("Counter","one",{n=21});local a=ws.publish(w)
w:set(x,"n",30);local b=ws.publish(w)
local wa=declared();local wb=declared();ws.open(a,wa);ws.open(b,wb)
assert(wa:ref("one").doubled==42 and wb:ref("one").doubled==60)
return {a=a:inspect(),b=b:inspect()}
''')["value"]
            assert r["a"]["node_id"] != r["b"]["node_id"] and r["a"]["sha256"] != r["b"]["sha256"]
        check("independent versions reopen without a mutable head", versions)

        def failure(mode, expected, effect, posts=0):
            f.reset(mode)
            r = run(IMPORT + DECL + 'return l.call("graphsub_world",{action="publish",snapshot=l.encode(w:snapshot())})')["value"]
            assert not r["ok"], r
            err = json.loads(r["result"])["error"]
            assert err["code"] == expected and err["effect"] == effect, err
            assert sum(method == "POST" for method, _ in f.requests) == posts, f.requests
            if mode == "redirect":
                assert len(f.requests) == 1, f.requests
            if mode in ("no_commit_progress", "tampered", "wrong_type", "wrong_identity"):
                assert "artifact" in err, err

        for mode, code in [("no_persistence", "persistence_unavailable"), ("no_manifest", "persistence_unavailable"),
                           ("malformed", "malformed_response"), ("duplicate", "malformed_response"),
                           ("precision", "precision_unsupported"), ("oversized", "response_too_large"),
                           ("not_found", "http_error"), ("redirect", "http_error")]:
            check("preflight rejects " + mode + " without writes", lambda m=mode, c=code: failure(m, c, "none"))
        for mode, code in [("lost_ack", "http_error"), ("bad_ack", "malformed_response"),
                           ("no_commit_progress", "persistence_unconfirmed"), ("tampered", "integrity_mismatch"),
                           ("wrong_identity", "malformed_response"), ("wrong_type", "malformed_response")]:
            check("publication " + mode + " records unknown effect without retry", lambda m=mode, c=code: failure(m, c, "unknown", 1))

        def invalid_input(value):
            f.reset()
            r = run(IMPORT + 'return l.call("graphsub_world",args)', args=value)["value"]
            assert not r["ok"] and "invalid_request" in r["result"], r
            assert not f.requests, f.requests

        base = {"action": "read", "node_id": "czBuMzAw", "sha256": "a" * 64, "world_id": "workspace-test"}
        for label, value in [("unknown field", base | {"url": host}), ("traversal", base | {"node_id": "../nodes"}),
                             ("string shard", base | {"shard_id": "0"}), ("fraction shard", base | {"shard_id": 0.5}),
                             ("bad digest", base | {"sha256": "abc"}), ("control world", base | {"world_id": "world\n"}),
                             ("wrong action", base | {"action": "update"}), ("mixed branch", base | {"snapshot": "{}"}),
                             ("oversized snapshot", {"action": "publish", "snapshot": "x" * 49153}),
                             ("invalid snapshot", {"action": "publish", "snapshot": "{}"})]:
            check("strict request rejects " + label, lambda v=value: invalid_input(v))

        def gate(env):
            f.reset()
            r = run(PUBLISH, env=env, success=False)
            assert next(iter(env)) in r["error"], r
            assert not f.requests, f.requests
        check("publication obeys write denial", lambda: gate({"DSCO_ALLOW_WRITE": "0"}))
        check("publication obeys network denial", lambda: gate({"DSCO_ALLOW_NET": "0"}))

        def false_option(expression):
            f.reset()
            result=run(IMPORT+DECL+'return ws.publish(w,'+expression+')',success=False)
            assert "lingo.workspace:" in result["error"] and not f.requests,result
        for label,expression in (("options","false"),("connection","{connection=false}"),("shard","{shard_id=false}")):
            check("false "+label+" is not silently defaulted",lambda e=expression:false_option(e))

        def network_allowlist():
            f.reset()
            result=run(PUBLISH,env={"DSCO_ALLOW_NET":"example.invalid"},success=False)
            assert not f.requests,result
        check("configured destination must satisfy the network allowlist",network_allowlist)

        def trifecta():
            f.reset()
            result=run(IMPORT+DECL+'''
assert(l.call("bash",{command='printf "%s\\n" "https://example.invalid/synthetic-untrusted"'}).ok)
assert(l.call("bash",{command='printf "%s\\n" "~/.ssh/id_rsa synthetic-secret-marker"'}).ok)
return ws.publish(w)
''',success=False)
            assert "lethal-trifecta" in result["error"] and not f.requests,result
        check("same-session secret plus untrusted taint still denies publication egress",trifecta)

        def duplicate_snapshot():
            f.reset()
            result=run(IMPORT+DECL+'''
local snapshot=l.encode(w:snapshot())
snapshot='{"world_id":"duplicate",'..snapshot:sub(2)
return l.call("graphsub_world",{action="publish",snapshot=snapshot})
''')["value"]
            assert not result["ok"] and "invalid_request" in result["result"] and not f.requests,result
        check("duplicate snapshot keys rejected before publication",duplicate_snapshot)

        def source_drift():
            f.reset()
            run(IMPORT + DECL + '''
w:new("Counter","one",{n=21});local a=ws.publish(w)
local changed=declared(nil,"2")
local ok,message=l.try(function()ws.open(a,changed)end)
assert(not ok and tostring(message):find("identity differs",1,true),tostring(message))
assert(#changed:objects("Counter")==0)
return true
''')
        check("source or definition drift cannot partially load objects", source_drift)

        def readonly_reopen():
            f.reset()
            # The identical complete source defines classes in both processes.
            source = IMPORT + DECL + '''
if args.address then ws.open(args.address,w);return w:ref("one").doubled end
w:new("Counter","one",{n=21});return ws.publish(w):inspect()
'''
            a = run(source, args={})["value"]
            f.reset()
            result = run(source, args={"address": a}, env={"DSCO_ALLOW_WRITE": "0"})
            assert result["value"] == 42 and all(method == "GET" for method, _ in f.requests)
        check("separate process reopens with write capability denied", readonly_reopen)

        def wrong_world():
            f.reset()
            r = run(IMPORT + DECL + 'return ws.open(args,declared("different"))', args=base | {"connection": "default", "shard_id": 0}, success=False)
            # Remove the direct-tool-only action for an artifact address.
            assert "unsupported field action" in r["error"] and not f.requests
            a = {k: v for k, v in base.items() if k != "action"} | {"connection": "default", "shard_id": 0}
            r = run(IMPORT + DECL + 'return ws.open(args,declared("different"))', args=a, success=False)
            assert "different world identity" in r["error"] and not f.requests
        check("wrong world or address shape fails before network", wrong_world)

        def pure_scope(kind):
            f.reset()
            source = IMPORT + DECL
            if kind == "scenario":
                source += 'return w:scenario("hypothetical",function()return ws.publish(w)end)'
            else:
                source += 'w:define("Effect",{value=l.derived("json",function()return ws.publish(w)end)});return w:new("Effect","effect",{}).value'
            result = run(source, success=False)
            assert "base script scope" in result["error"] and not f.requests, result
        check("calculation cannot publish", lambda: pure_scope("calculation"))
        check("scenario cannot publish", lambda: pure_scope("scenario"))

        def large():
            f.reset()
            source = IMPORT + DECL + 'w:new("Counter","one",{n=1,label=string.rep("x",36000)});local a,r=ws.publish(w);return r'
            r = run(source)["value"]
            assert len(r["snapshot"]) > 32768 and r["ok"]
        check("large bounded artifact remains complete valid JSON", large)

    report["status"] = "FAIL" if report["failed"] else "PASS"
    opt.report.parent.mkdir(parents=True, exist_ok=True)
    opt.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"status": report["status"], "passed": len(report["passed"]), "failed": report["failed"], "report": str(opt.report)}))
    return bool(report["failed"])


if __name__ == "__main__":
    raise SystemExit(main())
