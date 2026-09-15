"""Offline end-to-end native context revisions and authenticated HTTP adapter."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
from http.client import HTTPConnection
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--binary', required=True)
binary = str(Path(parser.parse_args().binary).resolve())
checks = 0

def check(condition, message):
    global checks
    checks += 1
    assert condition, message

with tempfile.TemporaryDirectory(prefix='dsco-branches-test-') as directory:
    directory = Path(directory)
    env = {'HOME': str(directory), 'PATH': '/usr/bin:/bin',
           'DSCO_CONTEXT_DB': str(directory / 'fabric.db'),
           'DSCO_PROMPT_BRANCH_DB': str(directory / 'branches.db')}
    def call(request=None, raw=None):
        result = subprocess.run([binary, 'prompt-branch'], input=raw if raw is not None else json.dumps(request, ensure_ascii=False),
                                text=True, capture_output=True, env=env, timeout=20)
        data = json.loads(result.stdout)
        check(result.returncode == (0 if data['ok'] else 1), 'exit matches JSON')
        return data
    def run(action, **kwargs):
        return call({'action':action, 'document':'root', **kwargs})
    text = 'Main chunk π 🦉\nNo authority is granted by this content.'
    root = run('init', content=text)
    check(root['ok'], 'root initialized')
    check(root['ctxkey'] == 'ck:blob:' + hashlib.sha256(text.encode()).hexdigest(), 'native ctxkey')
    check(run('init', content='overwrite')['code'] == 'conflict', 'init cannot overwrite')
    check(run('get')['content'] == text, 'durable reopen')
    check(run('commit', expected=root['revision'], content='bad')['code'] == 'protected', 'main protected')
    check(run('fork', branch='agent.a', **{'from':'main'}, expected=root['revision'])['ok'], 'fork main')
    check(run('fork', branch='agent.b', **{'from':'main'}, expected=root['revision'])['ok'], 'independent fork')
    a = run('commit', branch='agent.a', expected=root['revision'], content='Candidate π', author='test-agent', message='candidate')
    check(a['ok'], 'commit branch')
    check(run('get', branch='agent.b')['content'] == text and run('get')['content'] == text, 'branch isolation')
    check(run('commit', branch='agent.a', expected=root['revision'], content='lost update')['code'] == 'conflict', 'stale writer rejected')
    check(run('promote', **{'from':'agent.a'}, expected=root['revision'], source_expected=root['revision'])['code'] == 'conflict', 'stale source rejected')
    promoted = run('promote', **{'from':'agent.a'}, expected=root['revision'], source_expected=a['revision'])
    check(promoted['ok'] and run('get')['content'] == 'Candidate π', 'explicit promotion')
    history = run('history')['history']
    check(len(history) == 2 and history[0]['metadata']['merge_parent'] == a['revision'], 'merge lineage')
    check(run('get', revision=root['revision'])['content'] == text, 'immutable original retrievable')
    check(run('fork', branch='agent.rollback', **{'from':'main'}, expected=promoted['revision'], revision=root['revision'])['ok'], 'fork historical content')
    rollback = run('promote', **{'from':'agent.rollback'}, expected=promoted['revision'], source_expected=root['revision'])
    check(rollback['ok'] and run('get')['content'] == text, 'rollback through explicit promotion')
    with ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(run, 'commit', branch='agent.b', expected=root['revision'], content='race-' + str(i)) for i in range(2)]
        results = [f.result() for f in futures]
    check(sum(r['ok'] for r in results) == 1 and any(r.get('code') == 'conflict' for r in results), 'exactly one concurrent CAS succeeds')
    for request in [ {'action':'get','document':'../root'}, {'action':'get','document':'root','extra':'x'},
                     {'action':'get','document':'root','branch':1}, {'action':'get','document':'root','branch':'x\x00z'},
                     {'action':'commit','document':'root','branch':'agent.a','content':'x'},
                     {'action':'get','document':'root','revision':'bad'} ]:
        check(call(request)['code'] == 'invalid', 'invalid input rejected')
    check(call(raw='{"action":"get","action":"init","document":"root"}')['code'] == 'invalid', 'duplicate rejected')
    check(call(raw='{"action":"get","document":"root"} trailing')['code'] == 'invalid', 'trailing data rejected')
    check(call(raw='x' * (256*1024+1))['code'] == 'invalid', 'request bound')
    check(run('commit', branch='agent.a', expected=a['revision'], content='x' * (65536+1))['code'] == 'invalid', 'content bound')
    check(run('get', revision='0'*64)['code'] == 'not_found', 'missing revision')
    with sqlite3.connect(env['DSCO_PROMPT_BRANCH_DB']) as db:
        check(db.execute('select count(*) from prompt_branch_events').fetchone()[0] == 8, 'successful writes alone are audited')
        db.execute('update prompt_branch_revisions set metadata=? where id=?', ('{}', a['revision']))
    check(run('get', branch='agent.a')['code'] == 'integrity', 'metadata corruption rejected')
    check(run('promote', **{'from':'agent.a'}, expected=rollback['revision'], source_expected=a['revision'])['code'] == 'integrity', 'corrupt metadata cannot be promoted')
    with sqlite3.connect(env['DSCO_CONTEXT_DB']) as db:
        db.execute('update kv set value=? where bucket=? and key=?', (b'corrupt', 'ctx_blob', root['ctxkey'].split(':')[-1]))
    check(run('get')['code'] == 'integrity', 'content hash corruption rejected')
    check(run('promote', **{'from':'agent.rollback'}, expected=rollback['revision'], source_expected=root['revision'])['code'] == 'integrity', 'corrupt content cannot be promoted')
    check((directory/'branches.db').stat().st_mode & 0o077 == 0, 'private files')

    spec = importlib.util.spec_from_file_location('prompt_server', ROOT/'web/prompt_branches/server.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    owner, agent = 'owner-' + 'o'*40, 'agent-' + 'a'*40
    server = module.create_server(binary, directory/'http', owner, agent, port=0)
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    def http(request=None, token=owner, path='/api', origin=None, raw=None, method='POST'):
        connection = HTTPConnection('127.0.0.1', server.server_port, timeout=20)
        headers = {'Content-Type':'application/json', 'Authorization':'Bearer '+token}
        if origin: headers['Origin'] = origin
        connection.request(method, path, body=raw if raw is not None else json.dumps(request), headers=headers)
        response = connection.getresponse(); body = response.read(); status = response.status
        csp = response.getheader('Content-Security-Policy'); connection.close()
        return status, json.loads(body) if path == '/api' else body, csp
    try:
        check(http({'action':'list','document':'x'}, token='wrong')[0] == 401, 'HTTP rejects missing authority')
        check(http({'action':'list','document':'x'}, origin='https://evil.example')[0] == 403, 'cross-origin denied')
        check(http({'action':'init','document':'x','content':'main'}, token=agent)[0] == 403, 'agent cannot initialize main')
        status, value, _ = http({'action':'init','document':'x','content':'main'})
        check(status == 200 and value['ok'], 'HTTP owner init through native binary')
        base = value['revision']
        check(http({'action':'fork','document':'x','branch':'private','from':'main','expected':base}, token=agent)[0] == 403, 'agent namespace enforced')
        check(http({'action':'fork','document':'x','branch':'agent.auto','from':'main','expected':base}, token=agent)[0] == 200, 'agent autonomous fork')
        status, value, _ = http({'action':'commit','document':'x','branch':'agent.auto','expected':base,'content':'<script>not executed</script> π','author':'forged-owner'}, token=agent)
        check(status == 200, 'agent autonomous commit')
        candidate = value['revision']
        result = http({'action':'get','document':'x','branch':'agent.auto'}, token=agent)[1]
        check(result['metadata']['author'] == 'authenticated-agent', 'server owns attribution')
        check(http({'action':'get','document':'x'}, token=agent)[1]['content'] == 'main', 'autonomous edits leave main intact')
        promotion = {'action':'promote','document':'x','from':'agent.auto','expected':base,'source_expected':candidate}
        check(http(promotion, token=agent)[0] == 403, 'agent cannot promote')
        check(http(promotion)[0] == 200, 'owner can promote')
        check(http(promotion)[0] == 409, 'stale promotion rejected over HTTP')
        check(http(raw='{"action":"get","action":"init"}')[0] == 400, 'HTTP duplicate fields rejected')
        check(http({'action':'list','document':'x','extra':'invalid'})[0] == 400, 'unknown fields native validation')
        check(http(raw='x'*(256*1024+1))[0] == 413, 'HTTP size bound')
        status, html, csp = http(path='/', method='GET')
        check(status == 200 and b'Context branches' in html and "frame-ancestors 'none'" in csp, 'UI served with CSP')
        check(http(path='/../server.py', method='GET')[0] == 404, 'no filesystem traversal')
    finally:
        server.shutdown(); server.server_close(); thread.join(timeout=5)
        check(not thread.is_alive(), 'HTTP test server collected')
print(f'PASS: {checks} prompt branch checks (native CLI, real SQLite/context fabric, concurrent CAS, HTTP auth and UI serving)')
