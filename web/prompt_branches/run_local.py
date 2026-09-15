#!/usr/bin/env python3
"""Foreground local launcher; creates private tokens without printing them."""
import argparse
import json
import os
from pathlib import Path
import secrets
import stat
from server import create_server

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', required=True)
parser.add_argument('--data-dir', required=True)
parser.add_argument('--port', type=int, default=8791)
parser.add_argument('--public-origin')
args = parser.parse_args()
directory = Path(args.data_dir).resolve()
directory.mkdir(mode=0o700, parents=True, exist_ok=True)
if directory.stat().st_mode & 0o077:
    raise SystemExit('data directory must have mode 0700')
access = directory / 'access.json'
try:
    fd = os.open(access, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
except FileExistsError:
    pass
else:
    with os.fdopen(fd, 'w') as output:
        json.dump({'owner_token':secrets.token_urlsafe(48), 'agent_token':secrets.token_urlsafe(48)}, output)
        output.flush(); os.fsync(output.fileno())
fd = os.open(access, os.O_RDONLY | os.O_NOFOLLOW)
with os.fdopen(fd) as source:
    info = os.fstat(source.fileno())
    if not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077 or info.st_uid != os.getuid() or info.st_size > 4096:
        raise SystemExit('access.json must be a private, owned regular file <=4096 bytes')
    tokens = json.load(source)
server = create_server(args.binary, directory, tokens['owner_token'], tokens['agent_token'], args.port, args.public_origin)
print('Ready: http://127.0.0.1:' + str(server.server_port), flush=True)
print('Private token file: ' + str(access), flush=True)
try:
    server.serve_forever()
except KeyboardInterrupt:
    pass
finally:
    server.server_close()
