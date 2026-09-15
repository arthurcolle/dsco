#!/usr/bin/env python3
"""Isolated headless/PT Y integration checks; never open desktop windows."""
import os, pty, select, signal, subprocess, tempfile, time, json, pathlib, sys
BIN = str(pathlib.Path(sys.argv[1] if len(sys.argv)>1 else './dsco').resolve())
def run_ui(root, conflict=False):
    master, slave = pty.openpty()
    proc = subprocess.Popen([BIN,'ide',str(root)],stdin=slave,stdout=slave,stderr=slave,close_fds=True)
    os.close(slave)
    transcript = bytearray()
    def drain(seconds=.2):
        until=time.monotonic()+seconds
        while time.monotonic()<until:
            if select.select([master],[],[],.03)[0]:
                try: transcript.extend(os.read(master,65536))
                except OSError: break
    def send(b): os.write(master,b); drain()
    try:
        drain(.5)
        send(b'\x10'); send(b'file.txt\r'); send(b'X')
        if conflict: (root/'file.txt').write_text('external\n')
        send(b'\x13')
        if conflict:
            assert (root/'file.txt').read_text()=='external\n', bytes(transcript)[-1000:]
            send(b'\x18'); send(b'discard\r')
        else:
            assert (root/'file.txt').read_text()=='Xoriginal\n', bytes(transcript)[-1000:]
            assert (root/'file.txt').stat().st_mode & 0o777 == 0o640
        send(b'\x11')
        assert proc.wait(timeout=3)==0
    finally:
        if proc.poll() is None: proc.terminate(); proc.wait(timeout=3)
        os.close(master)
with tempfile.TemporaryDirectory(prefix='dsco-ide-check-') as temp:
    root=pathlib.Path(temp)
    (root/'file.txt').write_text('original\n'); (root/'file.txt').chmod(0o640)
    (root/'binary').write_bytes(b'abc\0def')
    (root/'link').symlink_to('file.txt')
    result=json.loads(subprocess.check_output([BIN,'ide','--check',temp]))
    assert result['ok']
    files={f['path']:f for f in result['files']}
    assert files['file.txt']['editable'] and not files['binary']['editable']
    assert 'link' not in files or not files['link']['editable']
    run_ui(root)
    (root/'file.txt').write_text('original\n')
    run_ui(root,True)
    assert subprocess.run([BIN,'ide',temp],stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE).returncode != 0
print('PASS: inventory, binary/symlink rejection, PTY edit/save, mode preservation, external-write conflict, clean exit, non-TTY rejection')
