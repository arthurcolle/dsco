#!/usr/bin/env python3
"""Observe incremental learning through the actual governed MCP dispatcher."""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
from test_swarm_interop_mcp import Mcp


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--binary', default='./dsco')
    p.add_argument('--output', required=True)
    args = p.parse_args()
    binary = Path(args.binary).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='dsco-progress-mcp-') as work:
        root = Path(work)
        env = dict(PATH='/usr/bin:/bin', HOME=work, DSCO_ENV_FILE='/dev/null',
                   DSCO_PRICING_OFFLINE='1', DSCO_SECURE_STORE_NO_PROMPT='1',
                   DSCO_TRACE='0', DSCO_GOV_MODEL='standard', DSCO_ALLOW_NET='0',
                   DSCO_ALLOW_RUN='0', DSCO_ALLOW_READ=work, DSCO_ALLOW_WRITE=work)
        client = Mcp(str(binary), root, env)
        def call(name, **arguments):
            return client.call('tools/call', dict(name=name, arguments=arguments))['result']
        def progress():
            result = call('self_improve', action='progress')
            assert not result.get('isError'), result
            return json.loads(result['content'][0]['text'])
        try:
            first = progress()
            assert first['evidence_revision'] == 0 and first['consolidations'] == 0, first
            assert first['weights']['cache_aggressiveness'] == 0.5, first
            for _ in range(12):
                assert not call('self_improve', action='consolidate').get('isError')
                assert progress() == first, 'recursive assessment manufactured evidence'
            for _ in range(3):
                result = call('read_file', path=str(root / 'missing.txt'))
                assert result.get('isError'), result
            pending = progress()
            assert pending['pending_evidence'] and pending['evidence_revision'] == 3, pending
            assert not call('self_improve', action='consolidate').get('isError')
            learned = progress()
            assert learned['consolidations'] == 1 and learned['suggestions'] > 0, learned
            assert learned['weights']['cache_aggressiveness'] > first['weights']['cache_aggressiveness']
            for _ in range(12):
                assert not call('self_improve', action='consolidate').get('isError')
                assert progress() == learned, 'unchanged evidence reinforced the prior'
            assert not call('self_improve', action='save').get('isError')
            history = json.loads((root / '.dsco/self_improve.json').read_text())
            assert history['schema'] == 'dsco.self_improve.history.v2'
            assert history['evidence_revision'] == 3 and history['consolidation_count'] == 1
            assert not learned['quality_verified'] and not learned['code_mutation']
            receipt = dict(ok=True, binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                           tool_calls=client.counter-1, first=first, pending=pending,
                           learned=learned, history=history, provider_requests=0)
        finally:
            client.close()
        Path(args.output).write_text(json.dumps(receipt, indent=2) + '\n')
        print(json.dumps(receipt))


if __name__ == '__main__':
    main()
