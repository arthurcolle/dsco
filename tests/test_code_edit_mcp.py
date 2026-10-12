#!/usr/bin/env python3
"""Actual production dispatch, isolated files and no provider inference."""
import argparse
import hashlib
import json
import pathlib
import tempfile
from test_swarm_interop_mcp import Mcp


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True)
    parser.add_argument('--artifact-dir', required=True)
    args = parser.parse_args()
    binary = pathlib.Path(args.binary).resolve()
    artifact = pathlib.Path(args.artifact_dir).resolve()
    artifact.mkdir(parents=True, exist_ok=True)
    receipts = []
    with tempfile.TemporaryDirectory(prefix='dsco-editor-mcp-') as work:
        root = pathlib.Path(work)
        allowed = root / 'allowed'
        allowed.mkdir()
        path = allowed / 'source.c'
        path.write_bytes('α\r\nbeta\r\nlast'.encode())
        path.chmod(0o751)
        env = dict(PATH='/usr/bin:/bin', HOME=work, DSCO_ENV_FILE='/dev/null',
                   DSCO_PRICING_OFFLINE='1', DSCO_SECURE_STORE_NO_PROMPT='1',
                   DSCO_GOV_MODEL='standard', DSCO_ALLOW_NET='0', DSCO_ALLOW_RUN='0',
                   DSCO_ALLOW_READ=str(allowed), DSCO_ALLOW_WRITE=str(allowed))
        client = Mcp(str(binary), root, env)
        def call(tool, expected_error=False, **params):
            error, receipt = client.tool(tool_name=tool, **params)
            receipts.append(dict(tool=tool, arguments=params, is_error=error, receipt=receipt))
            assert error == expected_error, receipt
            return receipt
        try:
            tools = client.call('tools/list', {})['result']['tools']
            by_name = {row['name']: row for row in tools}
            assert {'code_read', 'code_edit', 'skill'} <= by_name.keys()
            assert 'path' in by_name['code_read']['inputSchema']['properties']
            snapshot = call('code_read', path=str(path))
            assert snapshot['sha256'] == hashlib.sha256(path.read_bytes()).hexdigest()
            assert [line['text'] for line in snapshot['lines']] == ['α\r\n', 'beta\r\n', 'last']
            edit = dict(path=str(path), expected_sha256=snapshot['sha256'], edits=[
                dict(kind='replace', start=snapshot['lines'][1]['anchor'], new_text='new\r\n'),
                dict(kind='insert_after', start=snapshot['lines'][2]['anchor'], new_text='!')])
            call('code_edit', dry_run=True, **edit)
            assert path.read_bytes() == 'α\r\nbeta\r\nlast'.encode()
            changed = call('code_edit', **edit)
            assert changed['verified'] and changed['status'] == 'written'
            assert path.read_bytes() == 'α\r\nnew\r\nlast!'.encode()
            assert path.stat().st_mode & 0o777 == 0o751
            stale = call('code_edit', expected_error=True, **edit)
            assert stale['code'] == 'stale_snapshot'
            snapshot = call('code_read', path=str(path))
            overlap = [dict(kind='delete', start=snapshot['lines'][0]['anchor'])] * 2
            call('code_edit', expected_error=True, path=str(path), expected_sha256=snapshot['sha256'], edits=overlap)
            assert path.read_bytes() == 'α\r\nnew\r\nlast!'.encode()
            outside = root / 'outside.c'
            outside.write_text('untouched')
            call('code_read', expected_error=True, path=str(outside))
            call('code_edit', expected_error=True, path=str(outside), expected_sha256='absent', content='overwritten')
            assert outside.read_text() == 'untouched'
            link = allowed / 'link.c'
            link.symlink_to(outside)
            call('code_read', expected_error=True, path=str(link))
            path.write_bytes(b'a\x00b')
            call('code_read', expected_error=True, path=str(path))
            skill_path = allowed / 'SKILL.md'
            v1 = '---\nname: fixture\ndescription: Verify bounded edits.\n---\n# v1\n'
            v2 = v1.replace('# v1', '# v2 α')
            created = call('skill', action='create', path=str(skill_path), expected_sha256='absent', body=v1)
            invoked = call('skill', action='invoke', path=str(skill_path))
            assert invoked['sha256'] == created['sha256'] and invoked['instructions'] == v1
            assert invoked['activated'] is False and invoked['scope'] == 'invocation'
            updated = call('skill', action='update', path=str(skill_path), expected_sha256=created['sha256'], body=v2)
            assert pathlib.Path(updated['previous_path']).read_text() == v1
            assert skill_path.read_text() == v2
            call('skill', expected_error=True, action='update', path=str(skill_path), expected_sha256=created['sha256'], body=v1)
            call('skill', action='update', path=str(skill_path), expected_sha256=updated['sha256'], body=v1)
            assert skill_path.read_text() == v1
            call('skill', expected_error=True, action='create', path=str(allowed / 'bad.md'), expected_sha256='absent', body='bad')
        finally:
            client.close()
        # Explicit write denial outranks trusted tier, including the outer skill
        # verb and nested editor. Read-only invocation remains available.
        client = Mcp(str(binary), root, dict(env, DSCO_ALLOW_WRITE='0'))
        try:
            call('skill', action='invoke', path=str(skill_path))
            call('code_edit', expected_error=True, path=str(skill_path), expected_sha256=created['sha256'], content='denied')
            call('skill', expected_error=True, action='update', path=str(skill_path), expected_sha256=created['sha256'], body=v2)
            assert skill_path.read_text() == v1
        finally:
            client.close()
        # Narrow grants may permit the live file but deny its sibling revision.
        client = Mcp(str(binary), root, dict(env, DSCO_ALLOW_WRITE=str(skill_path)))
        try:
            skill_path.write_text(v1 + 'fresh\n')
            observed = call('skill', action='inspect', path=str(skill_path))
            call('skill', expected_error=True, action='update', path=str(skill_path), expected_sha256=observed['sha256'], body=v2)
            assert skill_path.read_text() == v1 + 'fresh\n'
            assert not (allowed / ('.SKILL.' + observed['sha256'] + '.md')).exists()
        finally:
            client.close()
        (artifact / 'mcp.stderr').write_text((root / 'mcp.stderr').read_text())
    (artifact / 'receipts.json').write_text(json.dumps(receipts, indent=2) + '\n')
    summary = dict(passed=True, calls=len(receipts), provider_inference=False,
                   binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                   checks=['anchors/CAS', 'exact CRLF/Unicode', 'metadata', 'preflight/overlap',
                           'binary/link rejection', 'read/write scopes', 'explicit write denial',
                           'skill invoke/create/update/history/rollback', 'nested history denial'])
    (artifact / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print('PASS: snapshot edits + skill lifecycle through actual MCP; %d calls; zero inference' % len(receipts))


if __name__ == '__main__':
    main()
