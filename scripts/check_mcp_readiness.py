#!/usr/bin/env python3
"""Read-only MCP readiness, not TCP availability. Never starts stdio servers.

Respects disabled entries and the runtime selector. No tool calls, credential
changes, redirects, OAuth registration or external account mutations.
"""
import argparse
import json
import os
from pathlib import Path
import urllib.error
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def classify(config, name, selector):
    if config.get('disabled') is True or config.get('enabled') is False:
        return 'disabled'
    if selector and selector not in ('*', 'all') and name not in selector.replace(',', ' ').split():
        return 'excluded_by_filter'
    if config.get('command') and not config.get('url'):
        return 'stdio_not_probed'
    return 'probe'


def parse_response(raw, request_id):
    if not raw.strip():
        return {}
    if raw.lstrip().startswith('{'):
        messages = [json.loads(raw)]
    else:
        messages = [json.loads(line[5:].strip()) for line in raw.splitlines() if line.startswith('data:')]
    return next((message for message in messages if message.get('id') == request_id), {})


def probe(config, timeout=15):
    headers = {k: os.path.expandvars(v) for k, v in config.get('headers', {}).items()}
    headers.update({k: os.path.expandvars(v) for k, v in config.get('http_headers', {}).items()})
    headers.update({'Content-Type': 'application/json', 'Accept': 'application/json, text/event-stream'})
    opener = urllib.request.build_opener(NoRedirect())

    def rpc(method, request_id=None, params=None):
        payload = {'jsonrpc': '2.0', 'method': method}
        if request_id is not None:
            payload['id'] = request_id
        if params is not None:
            payload['params'] = params
        request = urllib.request.Request(config['url'], data=json.dumps(payload).encode(), headers=headers)
        with opener.open(request, timeout=timeout) as response:
            session = response.headers.get('Mcp-Session-Id')
            if session:
                headers['Mcp-Session-Id'] = session
            body = response.read(8 * 1024 * 1024 + 1)
            if len(body) > 8 * 1024 * 1024:
                raise ValueError('response exceeds bound')
        return parse_response(body.decode(), request_id)

    try:
        init = rpc('initialize', 1, {'protocolVersion': '2024-11-05', 'capabilities': {},
                                    'clientInfo': {'name': 'dsco-readiness', 'version': '1'}})
        if not isinstance(init.get('result'), dict):
            return {'state': 'protocol_error', 'phase': 'initialize'}
        headers['MCP-Protocol-Version'] = init['result'].get('protocolVersion', '2024-11-05')
        rpc('notifications/initialized')
        tools = rpc('tools/list', 2, {})
        result = tools.get('result', {})
        if not isinstance(result, dict) or not isinstance(result.get('tools'), list):
            return {'state': 'protocol_error', 'phase': 'tools/list'}
        return {'state': 'connected', 'listed_tools': len(result['tools']),
                'more_pages': bool(result.get('nextCursor'))}
    except urllib.error.HTTPError as error:
        state = {401: 'authentication_required', 403: 'forbidden'}.get(error.code, 'http_error')
        # Never emit raw server messages, headers or URLs: they can contain tokens.
        status = error.code
        error.close()
        return {'state': state, 'http_status': status}
    except (OSError, ValueError, KeyError, TypeError):
        return {'state': 'transport_or_protocol_error'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=Path.home() / '.dsco/mcp.json')
    parser.add_argument('--server', default=os.environ.get('DSCO_MCP_SERVER') or os.environ.get('DSCO_MCP_SERVERS', ''))
    args = parser.parse_args()
    servers = json.loads(args.config.read_text())['mcpServers']
    results = []
    for name, config in servers.items():
        state = classify(config, name, args.server)
        result = probe(config) if state == 'probe' else {'state': state}
        results.append({'name': name, **result})
    connected = sum(r['state'] == 'connected' for r in results)
    failures = sum(r['state'] not in ('connected', 'disabled', 'excluded_by_filter', 'stdio_not_probed') for r in results)
    print(json.dumps({'servers': results, 'connected': connected, 'failed': failures,
                      'note': 'Catalog readiness only; no remote tool executed. Stdio servers not started.'}, indent=2))
    return 1 if failures else 0 if connected else 2


if __name__ == '__main__':
    raise SystemExit(main())
