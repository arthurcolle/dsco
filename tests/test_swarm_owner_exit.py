#!/usr/bin/env python3
"""No inference: returning from an owning runtime reaps its worker tree."""
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]

def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False

with tempfile.TemporaryDirectory(prefix="dsco-owner-exit-") as directory:
    root = Path(directory)
    ready = root / "ready.json"
    worker = root / "worker"
    # Nested tool deliberately has a different process group, as real tools do.
    worker.write_text('''#!/usr/bin/python3
import json, os, subprocess, time
child = subprocess.Popen(['/usr/bin/python3', '-c', 'import time; time.sleep(60)'], start_new_session=True)
row = {"schema":"dsco.inference_cost.v1", "provider_reported_usd":0.125,
       "estimated_inference_usd":None, "budget_accounted_usd":0.125}
os.write(int(os.environ['DSCO_WORKER_COST_FD']), (json.dumps(row)+'\\n').encode())
print('OWNER_EXIT_OUTPUT_RECEIPT', flush=True)
open(''' + repr(str(ready)) + ''', 'w').write(json.dumps([os.getpid(), child.pid]))
child.wait()
''')
    worker.chmod(0o700)
    pids = []
    try:
        env = os.environ.copy()
        env.update(DSCO_TEST_ONLY="swarm-owner-exit-child", DSCO_PRICING_OFFLINE="1",
                   DSCO_TEST_OWNER_SCRIPT=str(worker), DSCO_TEST_OWNER_READY=str(ready))
        env.pop("DSCO_SWARM_PRESERVE_CHILDREN", None)
        result = subprocess.run([str(ROOT / 'test_runner')], cwd=root, env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
        if ready.exists():
            pids = json.loads(ready.read_text())
        assert result.returncode == 0, result.stderr.decode()[-3000:]
        assert len(pids) == 2, "worker and nested tool must have started"
        for _ in range(100):
            if not any(alive(pid) for pid in pids):
                break
            time.sleep(.02)
        assert not any(alive(pid) for pid in pids), f"surviving owned processes: {pids}"
        rows = [json.loads(line) for line in (root / '.swarm/runs.jsonl').read_text().splitlines()]
        receipt = json.dumps(rows[-1])
        assert 'owner_exit' in receipt, receipt
        assert 'OWNER_EXIT_OUTPUT_RECEIPT' in receipt, "shutdown discarded worker output"
        saved = rows[-1]['workers'][0]
        assert saved['reported_cost_usd'] == .125, "shutdown discarded worker cost"
        assert saved['estimated_inference_cost_usd'] is None, "unknown estimate became zero"
        assert saved['status'] == 'cancelled', receipt
        assert rows[-1]['complete'] is True and rows[-1]['failed_workers'] == 1
        print('PASS: normal owner exit reaps native worker and nested tool; retains output and cost receipt')
        ready.unlink()
        env['DSCO_SWARM_PRESERVE_CHILDREN'] = '1'
        result = subprocess.run([str(ROOT / 'test_runner')], cwd=root, env=env,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
        if ready.exists():
            pids = json.loads(ready.read_text())
        assert result.returncode == 0
        assert len(pids) == 2 and all(alive(pid) for pid in pids), "explicit handoff killed workers"
        rows = [json.loads(line) for line in (root / '.swarm/runs.jsonl').read_text().splitlines()]
        assert rows[-1]['reason'] == 'owner_handoff' and rows[-1]['complete'] is False
        print('PASS: explicit preserve-children handoff retains workers; inherited fork cannot claim ownership')
    finally:
        for pid in reversed(pids):
            if alive(pid):
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                time.sleep(.05)
