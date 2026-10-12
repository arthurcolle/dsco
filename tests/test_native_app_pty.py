#!/usr/bin/env python3
"""Owned-PTY durable workbench checks. No desktop windows or inference requests."""
import argparse, fcntl, hashlib, json, os, pty, select, socket, stat, struct
import subprocess, tempfile, termios, threading, time
from pathlib import Path

class Workbench:
    launcher = False
    def __init__(self, binary, state, width=1200, zoom="0.65"):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH",45,100,width,900))
        env = os.environ.copy()
        env.update(TERM="xterm-kitty",DSCO_NATIVE_WORKBENCH_STATE=str(state),DSCO_PIXEL_TUI_ZOOM=zoom,DSCO_PIXEL_TUI_ANIMATIONS="0")
        for key in list(env):
            if key.endswith(("_API_KEY","_TOKEN")): env.pop(key, None)
        def setup():
            os.setsid(); fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
        argv = [str(binary)] + ([] if self.launcher else ["--native-workbench"])
        self.p = subprocess.Popen(argv,stdin=slave,stdout=slave,stderr=slave,env=env,preexec_fn=setup,close_fds=True)
        os.close(slave)
        self.path = Path(f"/tmp/dsco-workbench-{os.geteuid()}-{self.p.pid}.sock")
        self.done = threading.Event(); self.prefix = bytearray(); self.bytes = 0
        def drain():
            while not self.done.is_set():
                try:
                    if not select.select([self.master],[],[],.05)[0]: continue
                    block = os.read(self.master,65536)
                    if not block: break
                    self.bytes += len(block)
                    if len(self.prefix)<16000: self.prefix.extend(block[:16000-len(self.prefix)])
                except OSError: break
        self.reader = threading.Thread(target=drain,daemon=True); self.reader.start()
    def ready(self):
        until=time.monotonic()+10
        while time.monotonic()<until and self.p.poll() is None:
            if self.path.exists():
                try:
                    if self.rpc({"action":"apps"}).get("ok"): return
                except (OSError,ValueError): pass
            time.sleep(.025)
        raise AssertionError("workbench not ready: "+self.prefix.decode(errors="replace"))
    def rpc(self,obj):
        with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as sock:
            sock.settimeout(5); sock.connect(str(self.path)); sock.sendall(json.dumps(obj).encode()+b"\n"); parts=[]
            while True:
                chunk=sock.recv(65536)
                if not chunk: break
                parts.append(chunk)
        return json.loads(b"".join(parts))
    def apps(self): return self.rpc({"action":"apps"})["state"]["apps"]
    def windows(self): return self.rpc({"action":"list"})["windows"]
    def finished(self,app):
        until=time.monotonic()+5
        while time.monotonic()<until:
            found=next(a for a in self.apps() if a["id"]==app)
            if found["status"] in (2,3): return found
            time.sleep(.02)
        raise AssertionError("application did not finish")
    def quit(self):
        os.write(self.master,b"\x1b");time.sleep(.15);os.write(self.master,b"/quit\r")
        self.p.wait(timeout=7)
        assert self.p.returncode==0,(self.p.returncode,self.prefix.decode(errors="replace"))
        assert not self.path.exists()
    def close(self):
        if self.p.poll() is None:
            self.p.terminate()
            try: self.p.wait(timeout=4)
            except subprocess.TimeoutExpired: self.p.kill();self.p.wait(timeout=4)
        self.done.set();self.reader.join(timeout=1);os.close(self.master)
        if self.path.exists():self.path.unlink()

def main():
    parser=argparse.ArgumentParser();parser.add_argument("--binary",type=Path,required=True);parser.add_argument("--output",type=Path,required=True);parser.add_argument("--launcher",action="store_true");args=parser.parse_args()
    Workbench.launcher = args.launcher
    binary=args.binary.resolve();report={"binary":str(binary),"sha256":hashlib.sha256(binary.read_bytes()).hexdigest(),"scope":"owned PTY + native renderer/input + Unix RPC, not physical display observation","checks":[]}
    def record(name): report["checks"].append(name)
    with tempfile.TemporaryDirectory(prefix="dsco-app-pty-") as temp:
        state=Path(temp)/"apps.json"
        p=Workbench(binary,state)
        try:
            p.ready()
            until=time.monotonic()+5
            while not p.windows() and time.monotonic()<until: time.sleep(.02)
            first=p.windows()[0];assert stat.S_IMODE(p.path.stat().st_mode)==0o600
            assert p.rpc({"action":"stats"})["model_dispatch_supported"] is False
            assert not p.rpc({"action":"open","kind":"workflow"})["ok"]
            assert not p.rpc({"action":"define_factory","factory":"shell","title":"Shell","executor":"bash"})["ok"]
            assert p.rpc({"action":"define_factory","factory":"hash-custom","title":"Custom hash","executor":"sha256"})["ok"]
            made=p.rpc({"action":"factory","factory":"hash-custom","text":"unicode λ 📦"});assert made["ok"],made
            view=made["windows"][0];app=view["app_id"];wid=view["id"]
            p.rpc({"action":"focus","id":wid});time.sleep(.15);os.write(p.master,b"\x12")
            result=p.finished(app);assert result["input_sha256"]==hashlib.sha256("unicode λ 📦".encode()).hexdigest()
            record("real Ctrl-R executes compiled local function with Unicode input")
            assert p.rpc({"action":"close","id":wid})["ok"]
            assert next(a for a in p.apps() if a["id"]==app)["output"]==result["output"]
            reopened=p.rpc({"action":"reopen","app_id":app});assert reopened["ok"]
            wid=reopened["id"];assert wid!=view["id"]
            assert reopened["windows"][0]["result"]==result["output"]
            record("close/reopen preserves app ID, input and result with a new view ID")
            assert not p.rpc({"action":"reopen","app_id":app,"executor":"bash"})["ok"]
            # Human typing is persisted while background checkpoints run.
            assert p.rpc({"action":"update","id":wid,"text":"typed-"})["ok"]
            p.rpc({"action":"focus","id":wid});time.sleep(.1);os.write(p.master,b"Z");time.sleep(.2)
            typed=next(a for a in p.apps() if a["id"]==app)["input"];assert typed=="typed-Z",typed
            record("PTY text editing survives periodic persistence")
            for i in range(20):
                assert p.rpc({"action":"update","id":wid,"text":f"stream-{i}"})["ok"]
                assert p.rpc({"action":"run_task","id":wid})["ok"]
            result=p.finished(app);assert result["input"]=="stream-19"
            assert result["input_sha256"]==hashlib.sha256(b"stream-19").hexdigest()
            record("20 live input/run updates converge to newest state")
            fcntl.ioctl(p.master,termios.TIOCSWINSZ,struct.pack("HHHH",30,60,600,500));time.sleep(.15)
            assert p.rpc({"action":"resize","id":wid,"width":320,"height":220})["ok"]
            assert p.rpc({"action":"hide"})["ok"]
            assert p.rpc({"action":"run_task","id":wid})["ok"];p.finished(app)
            assert p.rpc({"action":"list"})["visible"] is False
            p.rpc({"action":"show"});record("narrow resize and hidden-view completion at zoom 0.65")
            assert p.rpc({"action":"close","id":first["id"]})["ok"]
            saved=p.rpc({"action":"checkpoint"});assert saved["ok"] and saved["durable"],saved
            disk=json.loads(state.read_text());assert stat.S_IMODE(state.stat().st_mode)==0o600
            assert next(a for a in disk["apps"] if a["id"]==app)["output"]==result["output"]
            # Second writer must fail before replacing the saved file.
            before=state.read_bytes();other=Workbench(binary,state)
            try:
                other.p.wait(timeout=5);assert other.p.returncode==2
                assert state.read_bytes()==before
            finally: other.close()
            record("explicit fsynced checkpoint, private permissions and competing-writer rejection")
            p.quit()
        finally: p.close()
        q=Workbench(binary,state,width=2000,zoom="1")
        try:
            q.ready();time.sleep(.15)
            windows=q.windows();assert len(windows)==1,windows
            assert windows[0]["app_id"]==app and windows[0]["result"]==result["output"]
            assert next(a for a in q.apps() if a["id"]==first["app_id"])["view_open"] is False
            assert q.rpc({"action":"run_task","id":windows[0]["id"]})["ok"]
            again=q.finished(app);assert again["input_sha256"]==result["input_sha256"]
            record("new process restores open/closed state and reruns saved immutable alias")
            q.quit()
        finally:q.close()
        state.write_text("{corrupt");before=state.read_bytes();r=Workbench(binary,state)
        try:
            r.p.wait(timeout=5);assert r.p.returncode==2;assert state.read_bytes()==before
            record("corrupt store refuses startup without overwriting evidence")
        finally:r.close()
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps(report,indent=2))
if __name__=="__main__":main()
