#!/usr/bin/env python3
"""Install/remove the periodic, headless, metadata-only catalog watch on macOS."""
import argparse
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile

LABEL = 'com.dsco.model-lab-watch'
ROOT = Path(__file__).resolve().parents[1]
PLIST = Path.home() / 'Library/LaunchAgents' / (LABEL + '.plist')
DOMAIN = 'gui/' + str(os.getuid())


def definition(interval=300):
    return {
        'Label': LABEL,
        'ProgramArguments': [sys.executable, str(ROOT / 'scripts/model_lab_watch.py')],
        'WorkingDirectory': str(ROOT),
        'StartInterval': interval,
        'RunAtLoad': True,
        'EnvironmentVariables': {'PATH': os.environ.get('PATH', '/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin')},
        'StandardOutPath': str(Path.home() / 'Library/Logs/dsco-model-lab-watch.log'),
        'StandardErrorPath': str(Path.home() / 'Library/Logs/dsco-model-lab-watch.err.log'),
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=('install', 'uninstall', 'status'))
    p.add_argument('--interval-seconds', type=int, default=300)
    a = p.parse_args()
    if not 60 <= a.interval_seconds <= 86400: p.error('interval must be in 60..86400 seconds')
    if sys.platform != 'darwin': raise SystemExit('LaunchAgent requires macOS')
    if a.action == 'status':
        result = subprocess.run(['launchctl', 'print', DOMAIN + '/' + LABEL], capture_output=True, text=True)
        print('loaded' if result.returncode == 0 else 'not_loaded', 'plist=' + str(PLIST))
        return
    if a.action == 'uninstall':
        if PLIST.exists(): subprocess.run(['launchctl', 'bootout', DOMAIN, str(PLIST)], capture_output=True)
        PLIST.unlink(missing_ok=True)
        print('removed=' + str(PLIST))
        return
    PLIST.parent.mkdir(parents=True, exist_ok=True)
    (Path.home() / 'Library/Logs').mkdir(exist_ok=True)
    payload = plistlib.dumps(definition(a.interval_seconds))
    loaded = subprocess.run(['launchctl', 'print', DOMAIN + '/' + LABEL], capture_output=True)
    if PLIST.exists() and PLIST.read_bytes() == payload:
        if loaded.returncode == 0:
            print('already_installed=' + str(PLIST)); return
    if PLIST.exists() and loaded.returncode == 0:
        result = subprocess.run(['launchctl', 'bootout', DOMAIN, str(PLIST)], capture_output=True, text=True)
        if result.returncode and 'Could not find service' not in result.stderr:
            raise SystemExit('Unable to unload prior job: ' + result.stderr[:200])
    with tempfile.NamedTemporaryFile(dir=PLIST.parent, prefix='.watch-', delete=False) as file:
        temp = Path(file.name)
        try:
            file.write(payload); file.flush(); os.fsync(file.fileno())
            os.chmod(file.name, 0o600)
            temp.replace(PLIST)
        finally: temp.unlink(missing_ok=True)
    result = subprocess.run(['launchctl', 'bootstrap', DOMAIN, str(PLIST)], capture_output=True, text=True)
    if result.returncode:
        PLIST.unlink(missing_ok=True)
        raise SystemExit('Unable to load model watch; no agent left installed: ' + result.stderr[:300])
    print('installed=' + str(PLIST), 'interval_seconds=' + str(a.interval_seconds), 'rollback=python3 scripts/model_lab_watch_schedule.py uninstall')


if __name__ == '__main__': main()
