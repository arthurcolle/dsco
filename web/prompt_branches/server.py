#!/usr/bin/env python3
"""Single-principal, loopback prompt branch adapter. TLS/auth proxy for hosting.

No LLM dispatch and no arbitrary command endpoint. Agents edit agent.* branches;
only the owner token may initialize documents or explicitly promote to main.
All content operations use the native DSCO context-fabric-backed CLI.
"""
import argparse
import hashlib
import hmac
from http.server import HTTPServer, BaseHTTPRequestHandler
import json
import os
from pathlib import Path
import subprocess
from urllib.parse import urlsplit

MAX_BODY = 256 * 1024
ASSETS = {'/': ('index.html', 'text/html; charset=utf-8'),
          '/app.js': ('app.js', 'text/javascript; charset=utf-8'),
          '/style.css': ('style.css', 'text/css; charset=utf-8')}
READS = {'get', 'list', 'history'}
ACTIONS = READS | {'init', 'fork', 'commit', 'promote'}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate JSON field')
        result[key] = value
    return result


def create_server(binary, data_dir, owner_token, agent_token, port=8791, origin=None):
    if min(len(owner_token), len(agent_token)) < 32 or owner_token == agent_token:
        raise ValueError('distinct owner/agent tokens of at least 32 characters required')
    binary = Path(binary).resolve(strict=True)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError('native DSCO binary must be executable')
    data_dir = Path(data_dir).resolve()
    data_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    if data_dir.stat().st_mode & 0o077:
        raise ValueError('data directory must be private (mode 0700)')
    env = {'HOME': str(data_dir), 'PATH': '/usr/bin:/bin',
           'DSCO_CONTEXT_DB': str(data_dir / 'fabric.db'),
           'DSCO_PROMPT_BRANCH_DB': str(data_dir / 'branches.db')}
    token_hashes = [(hashlib.sha256(t.encode()).digest(), role) for t, role in
                    ((owner_token, 'owner'), (agent_token, 'agent'))]
    public = None
    if origin:
        parsed = urlsplit(origin)
        if parsed.scheme != 'https' or not parsed.hostname or parsed.path not in ('', '/') or parsed.query or parsed.fragment or parsed.username or parsed.password:
            raise ValueError('public origin must be a plain HTTPS origin')
        public = origin.rstrip('/')

    class Handler(BaseHTTPRequestHandler):
        server_version = 'DSCO-PromptBranches/1'

        def setup(self):
            super().setup()
            self.connection.settimeout(5)

        def log_message(self, fmt, *args):
            pass  # never log prompts, tokens, or raw request lines

        def send_payload(self, status, body, content_type='application/json'):
            self.send_response(status)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.send_header('Referrer-Policy', 'no-referrer')
            self.send_header('Content-Security-Policy', "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'")
            self.end_headers()
            self.wfile.write(body)

        def reply(self, status, payload):
            self.send_payload(status, json.dumps(payload).encode())

        def origin_ok(self):
            local = 'http://127.0.0.1:' + str(self.server.server_port)
            allowed = {local}
            if public:
                allowed.add(public)
            hosts = {urlsplit(item).netloc for item in allowed}
            return (self.headers.get('Host') in hosts and
                    self.headers.get('Origin', local) in allowed and
                    len(self.headers.get_all('Host', [])) == 1)

        def do_GET(self):
            if not self.origin_ok():
                return self.reply(403, {'ok': False, 'code': 'origin'})
            asset = ASSETS.get(self.path)
            if not asset:
                return self.reply(404, {'ok': False, 'code': 'not_found'})
            self.send_payload(200, (Path(__file__).parent / asset[0]).read_bytes(), asset[1])

        def do_POST(self):
            if not self.origin_ok():
                return self.reply(403, {'ok': False, 'code': 'origin'})
            if self.path != '/api':
                return self.reply(404, {'ok': False, 'code': 'not_found'})
            auth = self.headers.get('Authorization', '')
            digest = hashlib.sha256(auth[7:].encode()).digest() if auth.startswith('Bearer ') else b''
            role = None
            for expected, candidate in token_hashes:
                if hmac.compare_digest(expected, digest):
                    role = candidate
            if not role or len(self.headers.get_all('Authorization', [])) != 1:
                return self.reply(401, {'ok': False, 'code': 'auth_required'})
            if self.headers.get('Transfer-Encoding') or len(self.headers.get_all('Content-Length', [])) != 1:
                return self.reply(400, {'ok': False, 'code': 'framing'})
            if self.headers.get('Content-Type', '').split(';')[0].strip() != 'application/json':
                return self.reply(415, {'ok': False, 'code': 'content_type'})
            try:
                length = int(self.headers['Content-Length'])
                if not 0 < length <= MAX_BODY:
                    return self.reply(413, {'ok': False, 'code': 'size'})
                raw = self.rfile.read(length)
                if len(raw) != length:
                    raise ValueError('incomplete body')
                request = json.loads(raw, object_pairs_hook=unique_object)
                if not isinstance(request, dict) or any(not isinstance(v, str) for v in request.values()):
                    raise ValueError('string-valued object required')
                action = request.get('action')
                if action not in ACTIONS:
                    raise ValueError('unknown action')
                if role == 'agent' and (action in {'init', 'promote'} or
                        (action not in READS and not request.get('branch', '').startswith('agent.'))):
                    return self.reply(403, {'ok': False, 'code': 'protected', 'error': 'agent token writes only agent.* branches; owner promotes main'})
                request['author'] = 'authenticated-' + role
                result = subprocess.run([str(binary), 'prompt-branch'],
                    input=json.dumps(request, ensure_ascii=False), text=True, capture_output=True,
                    timeout=15, env=env, cwd=str(data_dir), check=False)
                payload = json.loads(result.stdout)
                if not isinstance(payload, dict) or payload.get('ok') not in (True, False):
                    raise RuntimeError('invalid native response')
                status = 200 if payload.get('ok') and result.returncode == 0 else {
                    'conflict': 409, 'protected': 403, 'not_found': 404,
                    'busy': 503, 'storage': 503, 'integrity': 500}.get(payload.get('code'), 400)
                return self.reply(status, payload)
            except (ValueError, UnicodeError):
                return self.reply(400, {'ok': False, 'code': 'invalid'})
            except subprocess.TimeoutExpired:
                # The child may have committed before timeout. Never auto-retry writes.
                return self.reply(504, {'ok': False, 'code': 'outcome_unknown', 'error': 'read branch head before retrying'})
            except (OSError, RuntimeError):
                return self.reply(503, {'ok': False, 'code': 'backend_unavailable'})

    return HTTPServer(('127.0.0.1', port), Handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True)
    parser.add_argument('--data-dir', required=True)
    parser.add_argument('--port', type=int, default=8791)
    parser.add_argument('--public-origin', help='e.g. https://context.distributed.systems; does not configure DNS/TLS')
    args = parser.parse_args()
    server = create_server(args.binary, args.data_dir,
        os.environ.get('DSCO_PROMPT_OWNER_TOKEN', ''),
        os.environ.get('DSCO_PROMPT_AGENT_TOKEN', ''), args.port, args.public_origin)
    print('Prompt branches listening on http://127.0.0.1:' + str(server.server_port), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()

if __name__ == '__main__':
    main()
