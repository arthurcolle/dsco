#!/usr/bin/env python3
"""Native Lingo workflow boundary tests. HTTP fixtures are not service proof."""
import argparse
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
SPEC = {"id": "release", "name": "Release evidence", "version": "1", "timeout_seconds": 15,
        "steps": [{"id": "collect", "tool": "release:collect", "mode": "passthrough"},
                  {"id": "verify", "tool": "release:verify", "mode": "map",
                   "input_from": "collect", "input_mapping": {"capture": "$"}}]}
PREFIX = 'local l=require("lingo");local wf=require("lingo.workflow");'
EXECUTE = PREFIX + 'return wf.create(args.spec):execute{inputs={scope="current"},idempotency_key="proof-1"}'


@contextmanager
def server():
    state = {"mode": "happy", "requests": []}
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_): pass
        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            state["requests"].append((self.command, self.path, body))
            receipt = {"execution_id": "execution-1", "workflow_id": "release", "workflow_name": "Release evidence",
                       "execution_profile": "lingo.v1", "request_hash": "a" * 64, "status": "completed",
                       "step_results": {"collect": [{"hash": "one"}], "verify": [{"accepted": True}]},
                       "output": [{"accepted": True}], "duration_ms": 3.0, "trace_id": None, "idempotent_replay": False,
                       "idempotency_key":"proof-1","error":None}
            mode = state["mode"]
            if mode == "failed": receipt.update(status="failed", error="step_failed", output=None)
            if mode == "wrong_id": receipt["workflow_id"] = "other"
            if mode == "wrong_profile": receipt["execution_profile"] = "legacy"
            if mode == "precision": receipt["output"] = 9007199254740993
            if mode == "bad_hash": receipt["request_hash"]="z"*64
            if mode == "missing_steps": del receipt["step_results"]
            if mode == "missing_output": del receipt["output"]
            if mode == "missing_step": del receipt["step_results"]["verify"]
            if mode == "wrong_output": receipt["output"]={"different":True}
            if mode == "wrong_key": receipt["idempotency_key"]="other-key"
            if mode == "duplicate": data = b'{"status":"completed","status":"failed"}'
            elif mode == "oversize": data = b'"' + b'x' * (130 * 1024) + b'"'
            else: data = json.dumps(receipt).encode()
            self.send_response(503 if mode == "reject" else 200)
            self.send_header("Content-Length", str(len(data))); self.end_headers()
            try: self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError): pass
        def do_GET(self):
            state["requests"].append((self.command, self.path, None))
            if "/lingo/keys/" in self.path:
                value = {"execution_id":"execution-1","status":"completed","execution_profile":"lingo.v1",
                         "request_hash":"a"*64,"idempotency_key":"proof-1","idempotent_replay":True,
                         "workflow_id":"release","workflow_name":"Release evidence","step_results":{},
                         "output":None,"duration_ms":3,"error":None,"trace_id":None}
            else:
                value = {"execution_id": "execution-1", "workflow_id":"release","idempotency_key":"proof-1", "status": "completed", "steps": [],
                         "metadata": {"execution_profile": "lingo.v1", "request_hash": "a" * 64}}
            data = json.dumps(value).encode()
            self.send_response(200); self.send_header("Content-Length",str(len(data)));self.end_headers();self.wfile.write(data)
    http = ThreadingHTTPServer(("127.0.0.1",0),Handler)
    thread = threading.Thread(target=http.serve_forever,daemon=True);thread.start()
    try: yield state, f"http://127.0.0.1:{http.server_port}"
    finally: http.shutdown();http.server_close();thread.join()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary",nargs="?",type=Path,default=ROOT/"dsco")
    parser.add_argument("--report",type=Path)
    options=parser.parse_args();binary=options.binary.resolve(strict=True);checks=[]
    with tempfile.TemporaryDirectory(prefix="lingo-workflow-tests-") as cwd, server() as (state,url):
        env={k:os.environ[k] for k in ("PATH","HOME","TMPDIR","LANG") if k in os.environ}
        env.update(DSCO_ALLOW_RUN="1",DSCO_ALLOW_NET="1",DSCO_ALLOW_WRITE="1",DSCO_ALLOW_SECRETS="0",
                   DSCO_ALLOW_CONTROL="0",DSCO_ENV_FILE="/dev/null",DSCO_SECURE_STORE_NO_PROMPT="1",
                   DSCO_PRICING_OFFLINE="1",DSCO_NO_AUTO_SUPERVISE="1",DSCO_GOV_MODEL="standard",
                   TOOLS_API_URL=url,TOOLS_API_TOKEN="ephemeral-test-token",TOOLS_API_RETRIES="8",NO_PROXY="*")
        def run(source=EXECUTE,args=None,success=True,extra=None):
            p=subprocess.run([str(binary),"lingo","eval",source,json.dumps(args or {"spec":SPEC})],
                             cwd=cwd,env=env | (extra or {}),capture_output=True,text=True,timeout=20)
            assert (p.returncode==0)==success,(p.returncode,p.stdout,p.stderr)
            return json.loads(p.stdout)
        def check(name,callback):
            callback();checks.append({"name":name,"status":"PASS"})
        def require(ok): assert ok
        result=run();require(result["value"]["status"]=="completed")
        request=state["requests"][-1]
        check("native owner receives exact bounded profile",lambda:require(request[1]=="/api/v1/compose/execute"
              and request[2]["execution_profile"]=="lingo.v1" and request[2]["workflow"]["steps"][1]["name"]=="verify"
              and request[2]["workflow"]["max_parallel_steps"]==1))
        check("immutable workflow and defensive inspection",lambda:require(run(PREFIX+'''
          local w=wf.create(args.spec);local copy=w:inspect();copy.steps[1].tool="changed"
          local ok=l.try(function()w.bad=true end)
          return not ok and w:inspect().steps[1].tool=="release:collect"
        ''')["value"] is True))
        for mode in ("failed","wrong_id","wrong_profile","precision","duplicate","oversize","reject",
                     "bad_hash","missing_steps","missing_output","missing_step","wrong_output","wrong_key"):
            state["mode"]=mode;before=len(state["requests"])
            result=run(success=mode=="failed")
            require(len(state["requests"])==before+1)
            if mode=="failed":require(result["value"]["status"]=="failed")
            if mode=="reject":require("outcome_unknown" in json.dumps(result))
            checks.append({"name":mode+": preserves outcome and never retries","status":"PASS"})
        state["mode"]="happy"
        for grant in ("DSCO_ALLOW_NET","DSCO_ALLOW_WRITE","DSCO_ALLOW_RUN"):
            before=len(state["requests"]);run(success=False,extra={grant:"0"});require(len(state["requests"])==before)
            checks.append({"name":grant+" opt-out prevents dispatch","status":"PASS"})
        check("read receipt works with writes denied",lambda:require(run(PREFIX+'return wf.read("execution-1")',
              extra={"DSCO_ALLOW_WRITE":"0"})["value"]["execution_id"]=="execution-1"))
        check("reconcile key is a read and performs no resubmission",lambda:require(run(PREFIX+'return wf.reconcile("proof-1")',
              extra={"DSCO_ALLOW_WRITE":"0"})["value"]["idempotency_key"]=="proof-1" and state["requests"][-1][:2]==
              ("GET","/api/v1/compose/lingo/keys/proof-1")))
        run(PREFIX+'return wf.reconcile("wrong-key")',success=False)
        checks.append({"name":"reconcile refuses mismatched key receipt","status":"PASS"})
        invalids=[]
        for field,value in (("timeout_seconds",121),("timeout_seconds",True),("timeout_seconds",False),
                            ("version",False),("unknown",True),("steps",[])):
            spec=json.loads(json.dumps(SPEC));spec[field]=value;invalids.append(spec)
        spec=json.loads(json.dumps(SPEC));spec["steps"][1]["id"]="collect";invalids.append(spec)
        spec=json.loads(json.dumps(SPEC));spec["steps"][0]["input_from"]="verify";invalids.append(spec)
        spec=json.loads(json.dumps(SPEC));spec["steps"][0]["condition"]="True";invalids.append(spec)
        spec=json.loads(json.dumps(SPEC));spec["steps"][0]["mode"]="transform";invalids.append(spec)
        for field in ("mode","input_mapping","input_from"):
            spec=json.loads(json.dumps(SPEC));spec["steps"][0][field]=False;invalids.append(spec)
        spec=json.loads(json.dumps(SPEC));spec["steps"][0]["input_mapping"]=[];invalids.append(spec)
        spec=json.loads(json.dumps(SPEC));spec["steps"][0]["tool"]="with spaces";invalids.append(spec)
        for index,spec in enumerate(invalids):
            before=len(state["requests"]);run(args={"spec":spec},success=False);require(len(state["requests"])==before)
            checks.append({"name":f"invalid definition {index} rejected before I/O","status":"PASS"})
        for source in (PREFIX+'''local w=l.world{id="w"};w:define("A",{x=l.derived("json",function()
          return wf.create(args.spec):execute{inputs={},idempotency_key="bad"} end)});local a=w:new("A","a",{});return a.x''',
          PREFIX+'''local w=l.world{id="w"};return w:scenario("hypothetical",function()
          return wf.create(args.spec):execute{inputs={},idempotency_key="bad"} end)'''):
            before=len(state["requests"]);run(source,success=False);require(len(state["requests"])==before)
            checks.append({"name":"calculation/scenario cannot execute workflow","status":"PASS"})
        # Exercise native validation independently of the Lua builder.
        check("native rejects execution ID path injection",lambda:require(run(PREFIX+
            'return l.call("autobot_workflow",{action="read",execution_id="../other"}).ok')["value"] is False))
        for action,field in (("read","execution_id"),("reconcile","idempotency_key")):
            before=len(state["requests"])
            check("native rejects dot path for "+action,lambda:require(run(PREFIX+
                  'return l.call("autobot_workflow",{action="'+action+'",'+field+'=".."}).ok')["value"] is False))
            require(len(state["requests"])==before)
        check("native rejects conflicting action fields",lambda:require(run(PREFIX+
            'return l.call("autobot_workflow",{action="read",execution_id="execution-1",inputs={}}).ok')["value"] is False))
    report={"status":"PASS","checks":checks,"scope":"Actual native host against adversarial HTTP fixtures; no production service or workflow execution"}
    if options.report:options.report.parent.mkdir(parents=True,exist_ok=True);options.report.write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps({"status":"PASS","groups":len(checks)}))

if __name__=="__main__":main()
