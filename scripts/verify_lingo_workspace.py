#!/usr/bin/env python3
"""Publish two typed Lingo worlds to a fresh real GraphSub; kill, restart, reopen.

Only a private temporary engine directory is written. No existing service or
database is used. SIGKILL makes this a WAL/restart check, not a graceful-save
check. The wire receipt remains server-reported persistence; this independent
test documents observed recovery for the exact executed engine binary.
"""
from datetime import datetime, timezone
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("workspace_test",ROOT/"tests/test_lingo_workspace.py")
test=importlib.util.module_from_spec(spec)
spec.loader.exec_module(test)


def identity(path):
    path=Path(path).resolve()
    return {"path":str(path),"bytes":path.stat().st_size,
            "sha256":hashlib.sha256(path.read_bytes()).hexdigest()}


def ports():
    sockets=[socket.socket(),socket.socket()]
    try:
        for s in sockets:s.bind(("127.0.0.1",0))
        return [s.getsockname()[1] for s in sockets]
    finally:
        for s in sockets:s.close()


def http(url):
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(url,timeout=2) as response:
        assert response.status==200
        return json.load(response)


def stop(process, hard=False):
    if process and process.poll() is None:
        os.killpg(process.pid, signal.SIGKILL if hard else signal.SIGTERM)
        try:process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=5)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary",nargs="?",default=str(ROOT/"dsco"))
    parser.add_argument("--graphsub",type=Path,default=ROOT.parent/"dsco-graphsub-complex/dsco-graphsub/target/release/graphsub")
    parser.add_argument("--report-dir",type=Path,default=ROOT/"reports/lingo-world-20260910")
    opt=parser.parse_args()
    binary=Path(opt.binary).resolve();engine=opt.graphsub.resolve();directory=opt.report_dir.resolve()
    directory.mkdir(parents=True,exist_ok=True)
    workflow=ROOT/"examples/lingo/stored-world.lingo"
    sources=[ROOT/p for p in ("src/lingo_graphsub_world.c","src/lingo.c","src/capability.c",
              "src/service_boundary.c","lingo/runtime.lua","lingo/world_io.lua","lingo/workspace.lua","lingo/platform.lua")]
    inputs=[binary,engine,workflow,Path(__file__).resolve(),*sources]
    report={"status":"RUNNING","started_at":datetime.now(timezone.utc).isoformat(),
            "scope":"Actual isolated GraphSub engine and actual Lingo CLI; no fixture backend",
            "identity_note":"Executed binary hashes and observed source hashes; not a source-to-binary compiler attestation",
            "inputs":[identity(p) for p in inputs],"checks":[],"processes":[]}
    process=None
    logs=[]
    try:
        with tempfile.TemporaryDirectory(prefix="lingo-real-world-") as root:
            cwd=Path(root);tcp,port=ports();url=f"http://127.0.0.1:{port}"
            command=[str(engine),"start","--bind",f"127.0.0.1:{tcp}","--http",f"127.0.0.1:{port}",
                     "--persist",str(cwd/"data"),"--memory-max","64MB","--shard-count","1"]
            env={k:os.environ[k] for k in ("PATH","HOME","LANG","TMPDIR") if k in os.environ}
            report.update(command=command,isolated_root=str(cwd),url=url,auth="Native loopback engine; no platform gateway or credentials")

            def start(stage):
                stream=(directory/f"graphsub-{stage}.log").open("wb");logs.append(stream)
                child=subprocess.Popen(command,cwd=cwd,env=env,stdout=stream,stderr=subprocess.STDOUT,start_new_session=True)
                report["processes"].append({"stage":stage,"pid":child.pid})
                deadline=time.monotonic()+45
                while time.monotonic()<deadline:
                    if child.poll() is not None:raise RuntimeError(f"GraphSub exited {child.returncode}; see service log")
                    try:
                        health=http(url+"/health")
                        if health.get("service")=="graphsub" and health.get("status")=="healthy":
                            report.setdefault("health",{})[stage]=health
                            return child
                    except (OSError,ValueError):pass
                    time.sleep(.15)
                stop(child)
                raise RuntimeError("GraphSub health did not become ready")

            process=start("initial")
            published=test.invoke(binary,workflow,url,path=True,args={"action":"publish"})["value"]
            report["publication"]=published
            first,second=published["first"],published["second"]
            assert first["node_id"]!=second["node_id"] and first["sha256"]!=second["sha256"]
            report["checks"].append("two distinct content-verified artifact addresses published by real native REST")
            for name in ("first_receipt","second_receipt"):
                r=published[name]
                assert r["after"]["latest_committed_tx_id"]>r["before"]["latest_committed_tx_id"]
                assert r["after"]["commit_count"]>r["before"]["commit_count"]
                image=json.loads(r["snapshot"])
                assert all("eligible" not in obj["values"] for obj in image["objects"])
            report["checks"].append("server-reported WAL/commit counters advanced; no calculated values stored")
            report["persistence_before_crash"]=http(url+"/api/v1/shards/0/persistence")
            stop(process,hard=True)
            report["processes"][-1]["exit_code"]=process.returncode
            assert process.returncode==-signal.SIGKILL
            report["checks"].append("owned engine was killed without a graceful checkpoint")
            process=None
            report["data_files_after_crash"]=[{"path":str(p.relative_to(cwd)),"bytes":p.stat().st_size}
                                            for p in sorted(cwd.rglob("*")) if p.is_file()]
            process=start("restarted")
            report["persistence_after_restart"]=http(url+"/api/v1/shards/0/persistence")
            reopened={}
            for name,address,eligible in (("first",first,False),("second",second,True)):
                value=test.invoke(binary,workflow,url,path=True,args={"action":"open","address":address},
                                  env={"DSCO_ALLOW_WRITE":"0"})["value"]
                reopened[name]=value
                assert value["inspection"]["eligible"] is eligible
                assert value["inspection"]["object_count"]==4
                assert value["inspection"]["observation_payload"]["service"]=="graphsub"
                assert value["receipt"]["artifact"]==address
                assert value["receipt"]["snapshot"]==published[name+"_receipt"]["snapshot"]
            report["reopened"]=reopened
            report["checks"].append("fresh CLI processes reopened both versions after engine restart with WRITE=0")
            report["checks"].append("typed references and immutable observations survived; derived eligibility recomputed false/true")
            invalid=first|{"sha256":"0"*64}
            result=test.invoke(binary,workflow,url,path=True,args={"action":"open","address":invalid},success=False)
            assert "integrity_mismatch" in result["error"],result
            report["checks"].append("wrong content digest rejected by actual engine read binding")
            report["content_integrity_rejection"]=result
            end=[identity(p) for p in inputs]
            assert end==report["inputs"],"Execution inputs changed during proof"
            report["execution_inputs_unchanged"]=True
            report["status"]="PASS"
            stop(process)
            report["processes"][-1]["exit_code"]=process.returncode
            process=None
    except Exception as exc:
        report["status"]="FAIL";report["error"]=str(exc)
    finally:
        stop(process)
        for stream in logs:stream.close()
        report["all_owned_processes_stopped"]=process is None or process.poll() is not None
        report["finished_at"]=datetime.now(timezone.utc).isoformat()
        (directory/"real-restart-proof.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps({"status":report["status"],"checks":report["checks"],"error":report.get("error"),
                      "report":str(directory/"real-restart-proof.json")},indent=2))
    return report["status"]!="PASS"


if __name__=="__main__":raise SystemExit(main())
