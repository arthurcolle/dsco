#!/usr/bin/env python3
"""Run the resident coding model through DSCO's native governed agent loop."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from urllib.parse import urlsplit
from urllib.request import urlopen


def endpoint(value):
    error = 'Use a loopback HTTP endpoint ending in /v1 with a valid port and no control characters'
    if any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise argparse.ArgumentTypeError(error)
    value = value.rstrip('/')
    try:
        u = urlsplit(value)
        port = u.port
        valid = (u.scheme == 'http' and u.hostname in {'127.0.0.1', 'localhost', '::1'}
                 and not u.username and not u.password and not u.query and not u.fragment
                 and u.path == '/v1' and (port is None or 1 <= port <= 65535)
                 and not u.netloc.endswith(':'))
    except ValueError:
        valid = False
    if not valid:
        raise argparse.ArgumentTypeError(error)
    return value


def check(base):
    with urlopen(base[:-3] + '/ready', timeout=5) as response:
        state = json.load(response)
    if state.get('ready') is not True:
        raise RuntimeError('Coding model is not ready')
    with urlopen(base + '/models', timeout=5) as response:
        models = json.load(response)
    if not any(m.get('id') == state.get('model') for m in models.get('data', [])):
        raise RuntimeError('Readiness and served model inventory disagree')
    return {'ready': True, 'endpoint': base, 'model': state['model'],
            'pinned_bytes': state.get('pinned_bytes'), 'provider': 'mlx',
            'hosted_fallbacks': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prompt', nargs='?')
    parser.add_argument('--endpoint', type=endpoint,
                        default=os.environ.get('DSCO_CODER_ENDPOINT', 'http://127.0.0.1:18080/v1'))
    parser.add_argument('--directory', type=Path, default=Path.cwd())
    parser.add_argument('--dsco-bin', default=os.environ.get('DSCO_CODER_BIN') or shutil.which('dsco'))
    parser.add_argument('--check', action='store_true', help='Check the real service without inference')
    parser.add_argument('--verify', metavar='COMMAND', help='Required acceptance command, run through the native tool gate')
    parser.add_argument('--verify-timeout', type=int,
                        default=os.environ.get('DSCO_VERIFY_TIMEOUT', '60'))
    args = parser.parse_args()
    if not args.check and not args.prompt:
        parser.error('Supply a coding prompt or --check')
    if not args.directory.is_dir():
        parser.error('--directory must be an existing directory')
    if not args.check and (not args.dsco_bin or not Path(args.dsco_bin).is_file()
                           or not os.access(args.dsco_bin, os.X_OK)):
        parser.error('No executable DSCO binary found; supply --dsco-bin')
    try:
        status = check(args.endpoint)
    except Exception as error:
        print(json.dumps({'ready': False, 'error': str(error)}), file=sys.stderr)
        return 1
    if args.check:
        print(json.dumps(status, indent=2))
        return 0
    env = os.environ.copy()
    env.update({'MLX_API_BASE': args.endpoint, 'MLX_BASE_URL': args.endpoint,
                'DSCO_EXEC': 'mlx', 'DSCO_MODEL': 'mlx:default_model',
                'DSCO_DISABLE_DEFAULT_FALLBACKS': '1', 'DSCO_AUTO_FALLBACK': '0',
                'DSCO_SYSTEMS_AGENT': '0', 'DSCO_GOV_BYPASS': '0',
                'DSCO_ALLOW_CONTROL': '0', 'DSCO_ALLOW_SECRETS': '0',
                'DSCO_ALLOW_NET': '0', 'DSCO_MCP_HEADLESS': '0',
                'DSCO_PIXEL_TUI': '0', 'DSCO_AUTO_GOAL': '0',
                'DSCO_REQUIRE_TOOL_USE': '1'})
    for name in ('DSCO_LOCAL_FALLBACK_MODEL', 'DSCO_GOAL', 'DSCO_NET_FORCE'):
        env.pop(name, None)
    # Retain any stricter operator-supplied tool allowlist and capability grants.
    env.setdefault('DSCO_TOOL_ALLOWLIST',
                   'read_file,write_file,bash')
    env.setdefault('DSCO_BUDGET', '2')
    env.setdefault('DSCO_MAX_TOKENS', '4096')
    env.setdefault('DSCO_HARD_TURN_CEILING', '32')
    binary = str(Path(args.dsco_bin).resolve())
    trust = env.get('DSCO_TRUST_TIER', 'trusted')
    approval = env.get('DSCO_APPROVAL_MODE', 'never')
    if trust not in {'standard', 'trusted', 'untrusted'}:
        parser.error('Invalid DSCO_TRUST_TIER')
    if approval not in {'ask', 'strict', 'never'}:
        parser.error('Invalid DSCO_APPROVAL_MODE')
    if not args.verify or not args.verify.strip():
        parser.error('Coding runs require --verify COMMAND; model completion text is not acceptance')
    if not 1 <= args.verify_timeout <= 600:
        parser.error('--verify-timeout must be between 1 and 600 seconds')
    try:
        feature = subprocess.run([binary, '--profile', 'worker', '--execution-contract-version'],
                                 env=env, stdin=subprocess.DEVNULL, text=True,
                                 capture_output=True, timeout=5)
        protocol = json.loads(feature.stdout)
        if (feature.returncode or not isinstance(protocol, dict)
                or type(protocol.get('execution_contract_version')) is not int
                or protocol['execution_contract_version'] < 1):
            raise ValueError('unsupported execution contract')
    except (OSError, ValueError, subprocess.TimeoutExpired):
        parser.error('Selected DSCO binary lacks the verified execution contract; install the matching build')
    env['DSCO_VERIFY_COMMAND'] = args.verify
    env['DSCO_VERIFY_TIMEOUT'] = str(args.verify_timeout)
    governance = 'paranoid' if env.get('DSCO_GOV_MODEL') == 'paranoid' else 'standard'
    command = [binary, '--env-file', '/dev/null', '--profile', 'worker',
               '--provider', 'mlx', '--model', 'mlx:default_model', '--effort', 'none',
               '--gov-model', governance, '--trust-tier', trust,
               '--approval-mode', approval, '--prompt', args.prompt]
    os.chdir(args.directory)
    os.execve(binary, command, env)


if __name__ == '__main__':
    raise SystemExit(main())
