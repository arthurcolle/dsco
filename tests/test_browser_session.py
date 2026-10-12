#!/usr/bin/env python3
"""Real owned Chrome smoke, data URL fixture, offline proxy/DNS, no user profile.

Build separately from the shared repository build:
cc -std=c11 -D_DARWIN_C_SOURCE -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
  -Iinclude tests/browser_session_driver.c src/browser_session.c src/json_util.c \
  -lpthread -o /tmp/dsco-browser-session-test
python3 tests/test_browser_session.py --binary /tmp/dsco-browser-session-test
"""
import argparse
import base64
import json
import os
import pathlib
import select
import subprocess
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True)
    args = parser.parse_args()
    before_profiles = set(pathlib.Path('/tmp').glob('dsco-browser-*'))
    proc = subprocess.Popen([args.binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)
    session = tab = None
    checks = 0

    def call(action, expected=True, **kwargs):
        nonlocal checks
        request = dict(action=action, **kwargs)
        if session and action not in ('launch', 'status'):
            request.setdefault('session_id', session)
        if tab and action in ('navigate', 'snapshot', 'click', 'type', 'evaluate', 'screenshot'):
            request.setdefault('tab_id', tab)
        proc.stdin.write(json.dumps(request) + '\n')
        proc.stdin.flush()
        assert select.select([proc.stdout], [], [], 65)[0], f'no response: {action}'
        line = proc.stdout.readline()
        assert line, f'driver exited during {action}: {proc.stderr.read()}'
        result = json.loads(line)
        assert result['returned'] == expected, (request, result)
        assert result['response']['ok'] == expected, result
        checks += 1
        return result

    try:
        assert not call('status')['response']['result']['active']
        call('navigate', False, session_id='missing', tab_id='missing', url='about:blank')
        opened = call('launch', offline=True)['response']
        session = opened['session_id']
        tab = opened['result']['tabs'][0]['tab_id']
        state = call('status')['response']['result']
        assert state['active'] and state['headless'] and state['offline']
        chrome_pid = state['process_id']
        assert set(t['tab_id'] for t in call('tabs')['response']['result']['tabs']) == {tab}
        call('launch', False)
        call('tabs', False, session_id=session + '-wrong')
        call('snapshot', False, tab_id='not-a-tab')
        html = '''<!doctype html><html><head><title>Owned browser fixture</title></head>
        <body><h1>Browser session proof</h1><label for="name">Name</label><input id="name">
        <button id="go" onclick="document.querySelector('#answer').textContent='Hello '+document.querySelector('#name').value">Go</button>
        <output id="answer"></output><button class="ambiguous">A</button><button class="ambiguous">B</button>
        <input id="locked" readonly><button id="hidden" style="display:none">hidden</button>
        <p>Unicode: café 中文 ⧉</p></body></html>'''
        url = 'data:text/html;charset=utf-8;base64,' + base64.b64encode(html.encode()).decode()
        nav = call('navigate', url=url)['response']['result']
        assert nav['title'] == 'Owned browser fixture' and nav['ready_state'] == 'complete'
        snap = call('snapshot')['response']['result']
        assert 'Browser session proof' in snap['dom']['text']
        assert any(e['selector'] == '#name' for e in snap['dom']['elements'])
        assert any(e['role'] == 'button' and e['name'] == 'Go' for e in snap['accessibility'])
        call('type', selector='#name', text='Arthur 中文')
        call('click', selector='#go')
        observed = call('evaluate', expression='document.querySelector("#answer").textContent')['response']['result']
        assert observed == 'Hello Arthur 中文', observed
        call('type', selector='#name', text='Astra')
        assert call('evaluate', expression='document.querySelector("#name").value')['response']['result'] == 'Astra'
        call('click', False, selector='.ambiguous')
        call('click', False, selector='#hidden')
        call('type', False, selector='#locked', text='cannot')
        call('evaluate', False, expression='throw Error("fixture failure")')
        image = call('screenshot')
        assert image['image_base64_bytes'] > 1000 and image['response']['result']['image']
        started = time.monotonic()
        call('evaluate', False, expression='new Promise(()=>{})', timeout_ms=150)
        assert time.monotonic() - started < 3
        assert call('evaluate', expression='2 + 2')['response']['result'] == 4
        call('navigate', False, url='file:///etc/passwd')
        call('close', tab_id=tab)
        call('snapshot', False)
        call('close')
        assert not call('status')['response']['result']['active']
        try:
            os.kill(chrome_pid, 0)
            raise AssertionError('owned Chrome process survived close')
        except ProcessLookupError:
            pass
        profiles = set(pathlib.Path('/tmp').glob('dsco-browser-*')) - before_profiles
        assert not profiles, f'owned profile not removed: {profiles}'
        # A new lifetime must reject an ID from the prior lifetime.
        old_session = session
        reopened = call('launch', offline=True)['response']
        session = reopened['session_id']
        assert session != old_session
        call('tabs', False, session_id=old_session)
        call('close')
        print(f'PASS: {checks} real Chrome checks; DOM/AX, typed/clicked output, PNG, exact IDs, timeout recovery and cleanup')
    finally:
        proc.stdin.close()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        errors = proc.stderr.read()
        if errors:
            print(errors)
        assert proc.returncode == 0, proc.returncode


if __name__ == '__main__':
    main()
