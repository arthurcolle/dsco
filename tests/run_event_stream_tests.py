#!/usr/bin/env python3
"""Bounded launcher for the native process/IPC fixture."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
env = {key:value for key,value in os.environ.items() if not key.startswith('DSCO_EVENT_STREAM')}
results = []
for mode in ('multiprocess', 'slow', 'disconnected', 'failure'):
    with tempfile.TemporaryDirectory(prefix=f'dsco-event-{mode}-') as directory:
        proc = subprocess.run([binary, mode, str(Path(directory)/'events.sqlite')],
            env=env, text=True, capture_output=True, timeout=20)
        assert proc.returncode == 0, (mode, proc.returncode, proc.stdout, proc.stderr)
        result = json.loads(proc.stdout)
        assert result['passed'], result
        results.append(result)
        print('PASS', mode, flush=True)
print(json.dumps({'passed':True,'results':results},indent=2))
